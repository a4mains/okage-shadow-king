"""Stage postconditions: battle, encounter, Save, restore and strict-full exit
propagation. A scripted controller stands in for the native process; every
negative control proves a failure cannot be reported as success."""
import io
import json
import contextlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_native_route_support as support  # noqa: E402
from native_route import (battle, encounter, home, home_inside, outcome, pins, restore_check,  # noqa: E402
                          route, savefmt, strict_full)

def field_status(frame=100, scene=1, event=False, hero=(0.0, 0.0, 0.0), message=20107, choice=0, pc=512):
    return {'frame': frame, 'scene': scene, 'entrance': -1, 'event_running': event, 'pc': pc, 'message': message,
            'choice': choice, 'camera_yaw': 0.25, 'hero_position': list(hero) if hero is not None else None,
            'guard_pending_opcode': 0}

def observe(frame=100, mode=2, battle_=False, field=True, actors=()):
    return {'observe_frame': frame, 'game_mode': mode, 'battle': battle_, 'field': field, 'actors': list(actors)}

class FakeCtl:
    """Minimal scripted controller: observe()/status() come from callables of
    the tick count; step() advances ticks and may append scripted log lines."""

    def __init__(self, observe_fn=None, status_fn=None, log_at=None, alive=True):
        self.ticks = 0
        self.log = ''
        self.sent = []
        self.observe_fn = observe_fn or (lambda t: observe(t))
        self.status_fn = status_fn or (lambda t: field_status(t))
        self.log_at = dict(log_at or {})
        self._alive = alive

    def note(self, *a):
        pass

    def alive(self):
        return self._alive

    def log_offset(self):
        return len(self.log)

    def log_since(self, offset):
        return self.log[offset:]

    def roar_active(self):
        return False

    def send(self, *cmds):
        self.sent.extend(cmds)
        return []

    def step(self, n, mask=0):
        self.sent.append(f'step {n} {mask:04x}')
        for _ in range(n):
            self.ticks += 1
            if self.ticks in self.log_at:
                self.log += self.log_at.pop(self.ticks) + '\n'

    def observe(self):
        return self.observe_fn(self.ticks)

    def status(self):
        return self.status_fn(self.ticks)

    pick = staticmethod(lambda yaw, dx, dz, exclude=(): 0x1000)

class BattleTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.run = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_budget_exhausted_fails(self):
        c = FakeCtl(observe_fn=lambda t: observe(t, 3, True, False))
        with self.assertRaises(outcome.StageFailure) as e:
            battle.main(c, self.run, budget=200)
        self.assertEqual((e.exception.stage, e.exception.code), ('battle', 40))
        self.assertIn('budget', e.exception.reason)

    def test_never_constructed_fails(self):
        c = FakeCtl(observe_fn=lambda t: observe(t, 2, False, True))
        with self.assertRaises(outcome.StageFailure) as e:
            battle.main(c, self.run)
        self.assertIn('mode 3', e.exception.reason)

    def test_battle_ending_without_victory_fails(self):
        c = FakeCtl(observe_fn=lambda t: observe(t, 3, True, False) if t < 40 else observe(t, 2, False, True))
        with self.assertRaises(outcome.StageFailure) as e:
            battle.main(c, self.run)
        self.assertIn('without a native victory', e.exception.reason)

    def test_victory_passes(self):
        c = FakeCtl(observe_fn=lambda t: observe(t, 3, True, False) if t < 40 else observe(t, 2, False, True),
                    log_at={30: 'Native victory return queued scene=1 frame=130'})
        ev = battle.main(c, self.run)
        self.assertIn('victory return queued', ev['victory_line'])

    def test_fight_requires_field_return(self):
        c = FakeCtl(observe_fn=lambda t: observe(t, 3, True, False) if t < 40 else observe(t, 2, False, True),
                    status_fn=lambda t: field_status(t, scene=None, hero=None),
                    log_at={30: 'Native victory return queued scene=1 frame=130'})
        with self.assertRaises(outcome.StageFailure) as e:
            battle.fight_if_needed(c, self.run, result_confirms=5)
        self.assertIn('field did not return', e.exception.reason)

    def test_fight_success_requires_restore_line(self):
        def make(lines):
            return FakeCtl(observe_fn=lambda t: observe(t, 3, True, False) if t < 40 else observe(t, 2, False, True),
                           status_fn=lambda t: field_status(t) if t > 60 else field_status(t, scene=None, hero=None),
                           log_at=lines)
        with self.assertRaises(outcome.StageFailure):
            battle.fight_if_needed(make({30: 'Native victory return queued scene=1 frame=130'}), self.run)
        ev = battle.fight_if_needed(make({30: 'Native victory return queued scene=1 frame=130',
                                          50: 'Native saved field restored scene=1 frame=150'}), self.run)
        self.assertEqual(ev['field_after']['scene'], 1)

class EncounterTest(unittest.TestCase):
    def test_budget_exhausted_fails(self):
        c = FakeCtl()
        with self.assertRaises(outcome.StageFailure) as e:
            encounter.main(c, '.', budget=100)
        self.assertEqual((e.exception.stage, e.exception.code), ('encounter', 30))

    def test_field_gone_without_battle_fails(self):
        c = FakeCtl(status_fn=lambda t: field_status(t, scene=None, hero=None))
        with self.assertRaises(outcome.StageFailure) as e:
            encounter.main(c, '.')
        self.assertIn('field gone', e.exception.reason)

    def test_mode_change_without_contact_line_fails(self):
        c = FakeCtl(observe_fn=lambda t: observe(t, 2) if t < 30 else observe(t, 3, True, False))
        with self.assertRaises(outcome.StageFailure) as e:
            encounter.main(c, '.')
        self.assertIn('no native natural-encounter line', e.exception.reason)

    def test_contact_passes(self):
        c = FakeCtl(observe_fn=lambda t: observe(t, 2) if t < 30 else observe(t, 3, True, False),
                    log_at={25: 'Native natural encounter frame=125 row=1 column=3 formation=3 phase=11 rear=0'})
        self.assertIn('formation=3', encounter.main(c, '.')['contact_line'])

class SaveTest(unittest.TestCase):
    TARGET = 'bkmo2.dat'
    LOG = 'Native field Save constructed frame=93068\nHeadless ready\nNative field Save retired frame=93471\n'

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.run = Path(self.tmp.name)
        support.make_saves(self.run / 'saves')
        self.manifest = {'initial_saves': {p.name: {'sha256': pins.sha256(p), 'size': p.stat().st_size}
                                           for p in sorted((self.run / 'saves').iterdir())}}
        self.before = home_inside.saves_snapshot(self.run)
        self.after = field_status(93675, scene=74, hero=(4.42, 0, 0.32), pc=2952, message=10110)
        self.moved = field_status(93696, scene=74, hero=(7.44, 0, 6.19), pc=2952, message=10110)

    def tearDown(self):
        self.tmp.cleanup()

    def write_target(self, data=None):
        (self.run / 'saves' / self.TARGET).write_bytes(data if data is not None else support.save_bytes(9))

    def verify(self, **kw):
        args = dict(log_segment=self.LOG, after=self.after, moved=self.moved, alive=True)
        args.update(kw)
        return home_inside.verify_save(self.run, self.manifest, self.TARGET, self.before, **args)[1]

    def test_genuine_save_passes(self):
        self.write_target()
        self.assertEqual(self.verify(), [])

    def test_unchanged_target_fails(self):
        reasons = self.verify()
        self.assertTrue(any('unchanged' in r for r in reasons), reasons)

    def test_bad_checksum_fails(self):
        data = bytearray(support.save_bytes(9))
        data[100] ^= 1
        self.write_target(bytes(data))
        self.assertTrue(any('checksum' in r for r in self.verify()))

    def test_wrong_size_fails(self):
        self.write_target(support.save_bytes(9)[:-1])
        self.assertTrue(any('size' in r for r in self.verify()))

    def test_missing_log_lines_fail(self):
        self.write_target()
        self.assertTrue(self.verify(log_segment=''))
        self.assertTrue(self.verify(log_segment='Native field Save constructed frame=93068\n'))
        self.assertTrue(self.verify(log_segment=self.LOG + self.LOG))
        swapped = 'Native field Save retired frame=93471\nNative field Save constructed frame=93068\n'
        self.assertTrue(self.verify(log_segment=swapped))

    def test_dead_process_fails(self):
        self.write_target()
        self.assertIn('native process not alive after the Save', self.verify(alive=False))

    def test_not_back_in_field_control_fails(self):
        self.write_target()
        self.assertTrue(self.verify(after=field_status(scene=74, event=True)))
        self.assertTrue(self.verify(after=field_status(scene=None, hero=None)))
        self.assertTrue(self.verify(after=field_status(scene=73)))

    def test_hero_not_moving_fails(self):
        self.write_target()
        self.assertTrue(any('did not move' in r for r in self.verify(moved=self.after)))

    def test_non_target_change_or_leftover_temp_fails(self):
        self.write_target()
        (self.run / 'saves' / 'bkmo0.dat').write_bytes(support.save_bytes(7))
        self.assertTrue(any('non-target' in r for r in self.verify()))
        (self.run / 'saves' / 'bkmo0.dat').write_bytes(support.save_bytes(0))
        (self.run / 'saves' / '.bkmo2.tmp.1.1').write_bytes(b'x')
        self.assertTrue(any('unexpected new file' in r for r in self.verify()))

    def test_save_stage_without_file_change_raises(self):
        c = FakeCtl(status_fn=lambda t: field_status(t, scene=74, hero=(1.0 + t / 100, 0, 0), pc=2952,
                                                     message=10110, choice=2))
        with self.assertRaises(outcome.StageFailure) as e:
            home_inside.save(c, self.run, self.manifest, self.TARGET, 'tag')
        self.assertEqual((e.exception.stage, e.exception.code), ('save', 70))
        self.assertTrue(e.exception.details['reasons'])

    def test_save_stage_with_genuine_effects_passes(self):
        c = FakeCtl(status_fn=lambda t: field_status(t, scene=74, hero=(1.0 + t / 100, 0, 0), pc=2952,
                                                     message=10110, choice=2))
        orig = c.step
        done = []

        def step(n, mask=0):
            orig(n, mask)
            if c.ticks >= 500 and not done:
                done.append(1)
                self.write_target()
                c.log += self.LOG
        c.step = step
        ev = home_inside.save(c, self.run, self.manifest, self.TARGET, 'tag')
        self.assertEqual(ev['log'], {'constructed': [93068], 'retired': [93471]})

    def test_cursor_not_on_save_fails(self):
        c = FakeCtl(status_fn=lambda t: field_status(t, scene=74, choice=1))
        with self.assertRaises(outcome.StageFailure) as e:
            home_inside.save(c, self.run, self.manifest, self.TARGET, 'tag')
        self.assertIn('cursor', e.exception.reason)

    def test_target_changed_before_stage_fails(self):
        self.write_target()
        with self.assertRaises(outcome.StageFailure) as e:
            home_inside.save(FakeCtl(), self.run, self.manifest, self.TARGET, 'tag')
        self.assertIn('before the Save', e.exception.reason)

class RestoreCheckTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.run = Path(self.tmp.name)
        support.make_saves(self.run / 'saves')
        self.manifest = {'initial_saves': {p.name: {'sha256': pins.sha256(p)} for p in (self.run / 'saves').iterdir()}}

    def tearDown(self):
        self.tmp.cleanup()

    def test_payload_difference_outside_allowlist_fails(self):
        data = support.save_bytes(2)
        variables = bytearray(savefmt.payload(data)) + bytes(4096 - savefmt.PAYLOAD_SIZE)
        self.assertEqual(restore_check.payload_differences(data, bytes(variables)), [])
        variables[0x520] ^= 3
        diff = restore_check.payload_differences(data, bytes(variables))
        self.assertEqual([d['offset'] for d in diff], ['0x520'])
        variables[0x10] ^= 1
        c = FakeCtl()
        (self.run / 'control').mkdir()
        (self.run / 'control' / 'restored-field.png.variables.bin').write_bytes(bytes(variables))
        with mock.patch.object(restore_check.restore_route, 'main', return_value={}):
            with self.assertRaises(outcome.StageFailure) as e:
                restore_check.main(c, self.run, self.manifest, 'bkmo2.dat', 2)
        self.assertEqual((e.exception.stage, e.exception.code), ('restore-check', 90))
        self.assertIn('differ', e.exception.reason)

    def test_slot_checks(self):
        with self.assertRaises(outcome.StageFailure):
            restore_check.check_slot(self.run, self.manifest, 'bkmo9.dat')
        with self.assertRaises(outcome.StageFailure):
            restore_check.check_slot(self.run, self.manifest, 'bkmo2.dat', expect_sha='0' * 64)
        (self.run / 'saves' / 'bkmo2.dat').write_bytes(support.save_bytes(5))
        with self.assertRaises(outcome.StageFailure):
            restore_check.check_slot(self.run, self.manifest, 'bkmo2.dat')

    def test_no_victory_after_restore_fails(self):
        data = support.save_bytes(2)
        (self.run / 'control').mkdir()
        (self.run / 'control' / 'restored-field.png.variables.bin').write_bytes(savefmt.payload(data) + bytes(648))
        def scene(t):
            return 74 if t < 21 else 73 if t < 100 else 70 if t < 200 else 1
        c = FakeCtl(status_fn=lambda t: field_status(t, scene=scene(t), hero=(t / 10.0, 0, 0)))
        c.walk_to = lambda *a, **k: c.status()
        c.interact = lambda *a, **k: c.status()
        c.advance = lambda *a, **k: c.status()
        with mock.patch.object(restore_check.restore_route, 'main', return_value={}), \
                mock.patch.object(restore_check.navigate, 'navigate', side_effect=lambda c, *a, **k: c.status()):
            with self.assertRaises(outcome.StageFailure) as e:
                restore_check.main(c, self.run, self.manifest, 'bkmo2.dat', 2)
        self.assertIn('victory', e.exception.reason)
        self.assertNotIn('quit', c.sent)

def passing(name, ev=None):
    def fn(*a, **k):
        rec = k.get('record') or (a[-1] if a and callable(a[-1]) else None)
        if rec:
            rec(name, ev or {'ok': True})
        return ev or {'ok': True}
    return fn

def failing(stage, reason='synthetic'):
    def fn(*a, **k):
        raise outcome.StageFailure(stage, reason)
    return fn

class StrictFullTest(unittest.TestCase):
    """The historical strict_full.py exited with route.main's code even when
    home.main or home_inside.main failed. Every failure must surface here."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.run = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def go(self, route_fn=None, battle_fn=None, home_fn=None, inside_fn=None):
        patches = [mock.patch.object(strict_full.route, 'main', route_fn or passing('encounter')),
                   mock.patch.object(strict_full.battle, 'fight_if_needed', battle_fn or (lambda *a: {'victory': 1})),
                   mock.patch.object(strict_full.home, 'main', home_fn or (lambda *a: {'house': 1})),
                   mock.patch.object(strict_full.home_inside, 'main', inside_fn or passing('save'))]
        for p in patches:
            p.start()
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                code = strict_full.run_route(object(), self.run, {'_sha256': 'x'}, 'bkmo2.dat')
        finally:
            for p in patches:
                p.stop()
        return code, json.loads((self.run / 'route-result.json').read_text())

    def test_all_stages_pass(self):
        code, r = self.go()
        self.assertEqual((code, r['status'], r['exit_code']), (0, 'PASS', 0))
        self.assertEqual([s['stage'] for s in r['stages']], ['encounter', 'battle', 'home', 'save'])

    def test_home_failure_propagates(self):
        code, r = self.go(home_fn=failing('home', 'house interior (scene 70) not reached'))
        self.assertEqual((code, r['status'], r['failure']['stage']), (50, 'FAIL', 'home'))

    def test_save_failure_propagates(self):
        code, r = self.go(inside_fn=failing('save', 'Save postconditions failed'))
        self.assertEqual((code, r['failure']['stage']), (70, 'save'))

    def test_home_inside_climb_failure_propagates(self):
        code, r = self.go(inside_fn=failing('home-inside'))
        self.assertEqual(code, 60)

    def test_native_exit_inside_home_inside_is_a_failure(self):
        def boom(*a, **k):
            raise RuntimeError('native exited')
        code, r = self.go(inside_fn=boom)
        self.assertEqual((code, r['failure']['stage']), (60, 'home-inside'))
        self.assertIn('native exited', r['failure']['reason'])

    def test_bare_status_return_is_not_success(self):
        # Old-style `return 1` (or 0) from an unverified stage must not pass.
        for value in (1, 0, True):
            (self.run / 'route-result.json').unlink(missing_ok=True)
            code, r = self.go(home_fn=lambda *a: value)
            self.assertEqual((code, r['status']), (50, 'FAIL'))
            self.assertIn('bare status code', r['failure']['reason'])

    def test_missing_battle_after_contact_fails(self):
        code, r = self.go(battle_fn=lambda *a: None)
        self.assertEqual((code, r['failure']['stage']), (40, 'battle'))

    def test_battle_failure_inside_home_keeps_battle_code(self):
        code, r = self.go(home_fn=failing('battle', 'battle budget exhausted'))
        self.assertEqual((code, r['failure']['stage']), (40, 'battle'))

    def test_encounter_and_divergence_codes(self):
        code, r = self.go(route_fn=failing('encounter'))
        self.assertEqual(code, 30)
        self.assertEqual(r['stages'], [])

    def test_prelude_divergence_code(self):
        code, r = self.go(route_fn=failing('prelude-diverged'))
        self.assertEqual((code, r['failure']['stage']), (2, 'prelude-diverged'))

class RouteTest(unittest.TestCase):
    def test_rosalyn_segment_reads_pinned_run_tape(self):
        with tempfile.TemporaryDirectory() as d:
            run = Path(d)
            tape = ['status'] * 98 + ['step 4 0040', 'view x', 'status'] + ['step 1 0000'] * 192 + ['status']
            (run / 'historical-requests.txt').write_text('\n'.join(tape) + '\n')
            seg = route.rosalyn_segment(run, {'historical_prefix_count': len(tape)})
            self.assertEqual(seg[:2], ['step 4 0040', 'status'])
            self.assertNotIn('view x', seg)
            (run / 'historical-requests.txt').write_text('status\n' * 50)
            with self.assertRaises(outcome.StageFailure):
                route.rosalyn_segment(run, {'historical_prefix_count': 50})

    def test_route_has_no_absolute_evidence_paths(self):
        for name in ('route.py', 'home.py', 'home_inside.py', 'restore_check.py', 'session_driver.py', 'candidates.py'):
            text = (support.TOOLS / 'native_route' / name).read_text()
            self.assertNotIn("'/Users/", text, name)
            self.assertNotIn('local/research', text, name)

    def test_walk_without_event_fails(self):
        c = FakeCtl()
        c.walk_to = lambda *a, **k: field_status()
        with self.assertRaises(outcome.StageFailure) as e:
            route.walk(c)
        self.assertEqual(e.exception.code, 21)

    def test_home_not_reaching_house_fails(self):
        c = FakeCtl()
        c.interact = lambda *a, **k: field_status()
        with mock.patch.object(home.battle, 'fight_if_needed', return_value=None), \
                mock.patch.object(home.navigate, 'navigate', side_effect=lambda c, *a, **k: field_status()):
            with self.assertRaises(outcome.StageFailure) as e:
                home.main(c, '.', from_gate=True)
        self.assertEqual(e.exception.code, 50)

if __name__ == '__main__':
    unittest.main()
