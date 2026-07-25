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

    def test_victory_line_after_the_battle_is_freed_passes(self):
        # claude-final-restore-roar-v2b: battle freed at 12217, line follows.
        c = FakeCtl(observe_fn=lambda t: observe(t, 3, True, False) if t < 40 else observe(t, 3, False, False),
                    log_at={45: 'Native victory return queued scene=1 frame=145'})
        self.assertIn('victory return queued', battle.main(c, self.run)['victory_line'])

    def test_defeat_after_the_battle_is_freed_fails_without_waiting(self):
        c = FakeCtl(observe_fn=lambda t: observe(t, 3, True, False) if t < 40 else observe(t, 3, False, False),
                    log_at={42: 'Native defeat return requested mode=8 arg=4 frame=142',
                            100: 'Native victory return queued scene=1 frame=200'})
        with self.assertRaises(outcome.StageFailure) as e:
            battle.main(c, self.run)
        self.assertIn('without a native victory', e.exception.reason)
        self.assertLess(c.ticks, 100)

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

    def test_field_freed_after_contact_before_mode_change_passes(self):
        c = FakeCtl(observe_fn=lambda t: observe(t, 2, field=t < 30),
                    status_fn=lambda t: field_status(t) if t < 30 else field_status(t, scene=None, hero=None),
                    log_at={25: 'Native natural encounter frame=125 row=1 column=3 formation=4 phase=11 rear=0'})
        ev = encounter.main(c, '.')
        self.assertIn('formation=4', ev['contact_line'])
        self.assertEqual(ev['observe_frame'], 30)

    def test_field_freed_with_contact_before_patrol_fails(self):
        c = FakeCtl(observe_fn=lambda t: observe(t, 2, field=False),
                    status_fn=lambda t: field_status(t, scene=None, hero=None))
        c.log = 'Native natural encounter frame=1 row=1 column=3 formation=4 phase=11 rear=0\n'
        with self.assertRaises(outcome.StageFailure) as e:
            encounter.main(c, '.')
        self.assertIn('field gone', e.exception.reason)

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
        c.walk_to = lambda *a, **k: field_status()
        with mock.patch.object(home.battle, 'fight_if_needed', return_value=None), \
                mock.patch.object(home.navigate, 'navigate', side_effect=lambda c, *a, **k: field_status()):
            with self.assertRaises(outcome.StageFailure) as e:
                home.main(c, '.', from_gate=True)
        self.assertEqual(e.exception.code, 50)

    def test_home_from_gate_walks_the_yard_waypoints(self):
        c = FakeCtl()
        walked = []
        c.walk_to = lambda x, z, **k: walked.append((x, z))
        c.interact = lambda *a, **k: field_status()
        c.status = lambda: field_status(hero=walked[-1][:1] + (0.0,) + walked[-1][1:] if walked else (181.9, 0, 710.0))
        with mock.patch.object(home.battle, 'fight_if_needed', return_value=None), \
                mock.patch.object(home.navigate, 'navigate', side_effect=lambda c, *a, **k: field_status()):
            with self.assertRaises(outcome.StageFailure):
                home.main(c, '.', from_gate=True)
        self.assertEqual(walked[:len(home.YARD)], home.YARD)

class RestoreCheckCliTests(unittest.TestCase):
    def test_restore_check_requires_the_new_save_hash(self):
        # Review finding 4: without the expected hash any pinned save could pass.
        from native_route import cli
        parser = cli.build_parser()
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            parser.parse_args(['restore-check', '/tmp/run', '--slot-file', 'bkmo2.dat', '--slot-rights', '2'])
        a = parser.parse_args(['restore-check', '/tmp/run', '--slot-file', 'bkmo2.dat', '--slot-rights', '2',
                               '--expect-save-sha256', 'ab' * 32])
        self.assertEqual(a.expect_save_sha256, 'ab' * 32)

class RestEveryCliTests(unittest.TestCase):
    def test_rest_every_requires_until_roar(self):
        from native_route import cli
        base = ['restore-check', '/tmp/run', '--slot-file', 'bkmo2.dat', '--slot-rights', '2',
                '--expect-save-sha256', 'ab' * 32, '--rest-every', '2']
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as e:
            cli.parse(base)
        self.assertEqual(e.exception.code, cli.USAGE)
        self.assertEqual(cli.parse(base + ['--until-roar', '5']).rest_every, 2)

class RoarEncountersTests(unittest.TestCase):
    def test_roar_phase_requires_a_won_roar_battle(self):
        # Never passes on ordinary victories alone; needs a roar line before a victory.
        from native_route import restore_check
        logs = {'text': ''}
        class C:
            def log_offset(self): return 0
            def log_since(self, start): return logs['text']
            def status(self): return field_status(scene=1)
        with mock.patch.object(restore_check.navigate, 'navigate', return_value=None):
            logs['text'] = 'Native natural encounter frame=1 row=1\nNative victory return queued scene=1 frame=9\n'
            with self.assertRaises(outcome.StageFailure):
                restore_check.roar_encounters(C(), '/tmp/x', 1)
            # A roar won only by a later, different battle does not count.
            logs['text'] += ('Native natural encounter frame=19 row=1 column=1 formation=3 phase=8 rear=0\n'
                             'Native natural encounter message id=100004 frame=20\n'
                             'Native natural encounter frame=31 row=1 column=2 formation=4 phase=11 rear=0\n'
                             'Native victory return queued scene=1 frame=40\n')
            with self.assertRaises(outcome.StageFailure):
                restore_check.roar_encounters(C(), '/tmp/x', 2)
            # Real order: phase-8 contact, roar message, the roar battle's victory.
            logs['text'] += ('Native natural encounter frame=59 row=1 column=1 formation=3 phase=8 rear=0\n'
                             'Native natural encounter message id=100004 frame=60\n'
                             'Native victory return queued scene=1 frame=80\n')
            r = restore_check.roar_encounters(C(), '/tmp/x', 3)
            self.assertTrue(any('message id=100004' in e for e in r['events']))

class RestTests(unittest.TestCase):
    """Ordinary bedroom Rest (heal between roar-coverage patrols)."""
    REST = 'Native field Rest recovered party frame=300'

    def ctl(self, choice=0, log_at=None, back=True):
        # Menu event in the bedroom until tick 400, then field control (or never).
        def status(t):
            if t < 400 or not back:
                return field_status(t, scene=74, event=True, choice=choice)
            return field_status(t, scene=74)
        c = FakeCtl(status_fn=status, log_at=log_at)
        c.advance = lambda until, **k: c.status()
        return c

    def test_cursor_not_on_rest_fails(self):
        with self.assertRaises(outcome.StageFailure) as e:
            home_inside.rest(self.ctl(choice=2, log_at={300: self.REST}), 't')
        self.assertEqual((e.exception.stage, e.exception.code), ('rest', 75))
        self.assertIn('not on Rest', e.exception.reason)

    def test_no_recovery_line_fails(self):
        with self.assertRaises(outcome.StageFailure) as e:
            home_inside.rest(self.ctl(), 't')
        self.assertIn('no native party recovery', e.exception.reason)

    def test_recovery_before_the_confirm_does_not_count(self):
        with self.assertRaises(outcome.StageFailure):
            home_inside.rest(self.ctl(log_at={100: self.REST}), 't')

    def test_no_field_control_after_rest_fails(self):
        with self.assertRaises(outcome.StageFailure) as e:
            home_inside.rest(self.ctl(log_at={300: self.REST}, back=False), 't')
        self.assertIn('field control', e.exception.reason)

    def test_rest_passes(self):
        c = self.ctl(log_at={300: self.REST})
        ev = home_inside.rest(c, 't')
        self.assertEqual(ev['recovered'], [self.REST])
        self.assertEqual(ev['after']['scene'], 74)
        self.assertIn('step 1 0040', c.sent)

class RoarRestCycleTests(unittest.TestCase):
    def run_loop(self, script, rest_every, max_victories=20):
        """script: log text appended per patrol leg."""
        from native_route import restore_check
        logs = {'text': '', 'leg': 0}
        class C:
            def log_offset(self): return 0
            def log_since(self, start): return logs['text']
            def status(self): return field_status(scene=1)
        def leg(*a, **k):
            if logs['leg'] < len(script):
                logs['text'] += script[logs['leg']]
            logs['leg'] += 1
        cycles = []
        with mock.patch.object(restore_check.navigate, 'navigate', side_effect=leg), \
                mock.patch.object(restore_check, 'rest_cycle', side_effect=lambda c, run, n: cycles.append(n) or {'n': n}):
            try:
                return restore_check.roar_encounters(C(), '/tmp/x', max_victories, rest_every), cycles
            except outcome.StageFailure as e:
                return e, cycles

    WIN = 'Native natural encounter frame=1 row=1\nNative victory return queued scene=1 frame=9\n'
    ROAR = ('Native natural encounter frame=19 row=1 column=1 formation=3 phase=8 rear=0\n'
            'Native natural encounter message id=100004 frame=20\n')
    ROAR_WIN = ROAR + 'Native victory return queued scene=1 frame=40\n'

    def test_rests_after_every_n_victories_until_a_won_roar(self):
        r, cycles = self.run_loop([self.WIN, self.WIN, self.WIN, self.WIN, self.WIN, self.ROAR_WIN], 2)
        self.assertNotIsInstance(r, outcome.StageFailure)
        self.assertEqual(cycles, [1, 2])
        self.assertEqual(len(r['rests']), 2)

    def test_roar_followed_by_another_battle_is_not_a_won_roar(self):
        r, _ = self.run_loop([self.ROAR, self.WIN, self.WIN], 0, max_victories=2)
        self.assertIsInstance(r, outcome.StageFailure)

    def test_no_rest_without_option_and_victories_alone_never_pass(self):
        r, cycles = self.run_loop([self.WIN] * 5, 0, max_victories=5)
        self.assertIsInstance(r, outcome.StageFailure)
        self.assertEqual((r.stage, cycles), ('roar-encounters', []))

class NavigateRoarTests(unittest.TestCase):
    def test_settle_handles_an_active_roar_while_the_hero_is_locked(self):
        from native_route import navigate
        c = FakeCtl()
        state = {'roar': True}
        c.roar_active = lambda: state['roar']
        fought = []
        def fight(c, run):
            fought.append(1)
            state['roar'] = False
        with mock.patch.object(navigate.battle, 'fight_if_needed', side_effect=fight):
            self.assertIsNone(navigate.settle(c, '.', 1))
        self.assertEqual(fought, [1])

    def test_settle_without_roar_returns_at_once(self):
        from native_route import navigate
        c = FakeCtl()
        with mock.patch.object(navigate.battle, 'fight_if_needed') as fight:
            self.assertIsNone(navigate.settle(c, '.', 1))
        fight.assert_not_called()

class RoarWheelTests(unittest.TestCase):
    """pilot.Ctl.roar against a model of the recovered Stan roar dialogue model: text
    pages advance on Confirm; a QC wheel needs Up/Down to arm (no cursor move),
    then Confirm takes the cursor row and disarms; Down while armed moves it."""

    class Model:
        def __init__(self, pages):
            self.pages = pages  # 'text' or 'qc' per text_progress step
            self.progress, self.armed, self.cursor, self.rows, self.frame = 0, False, 0, [], 0
            self.notes = []

        def roar_active(self):
            return self.progress < len(self.pages)

        def observe(self):
            return {'battle': False, 'field': True, 'message': 100011, 'text_progress': self.progress,
                    'choice': self.cursor, 'observe_frame': self.frame}

        def note(self, *a):
            self.notes.append(a)

        def step(self, n, mask=0):
            from native_route.pilot import CONFIRM, DOWN
            self.frame += n
            if not self.roar_active():
                return
            page = self.pages[self.progress]
            if mask & DOWN and page == 'qc':
                if self.armed:
                    self.cursor = (self.cursor + 1) % 3
                self.armed = True
            elif mask & CONFIRM:
                if page == 'text':
                    self.progress += 1
                elif self.armed:
                    self.rows.append(self.cursor)
                    self.armed, self.cursor = False, 0
                    self.progress += 1

    PAGES = ['text', 'qc', 'qc', 'qc', 'text', 'text']

    def test_roar_arms_each_wheel_and_takes_the_default_rows(self):
        from native_route import pilot
        m = self.Model(self.PAGES)
        pilot.Ctl.roar(m)
        self.assertFalse(m.roar_active())
        self.assertEqual(m.rows, [0, 0, 0])

    def test_a_down_that_moves_the_cursor_fails(self):
        # A wheel that keeps rejecting Confirm (flag 0x10 clear) makes the
        # handler re-arm; Down on an armed wheel moves the cursor -> failure.
        from native_route import pilot
        m = self.Model(['qc'])
        real = m.step
        def step(n, mask=0):
            from native_route.pilot import CONFIRM
            if mask & CONFIRM:
                m.frame += n
                return
            real(n, mask)
        m.step = step
        with self.assertRaises(outcome.StageFailure) as e:
            pilot.Ctl.roar(m)
        self.assertIn('moved the roar wheel cursor', e.exception.reason)

    def test_confirm_alone_never_passes_a_wheel(self):
        # The pre-fix pilot.roar (claude-final-restore-roar-v2d 72880..81152, Confirm only).
        from native_route.pilot import CONFIRM
        m = self.Model(self.PAGES)
        for _ in range(50):
            m.step(4, CONFIRM)
        self.assertEqual(m.progress, 1)

if __name__ == '__main__':
    unittest.main()
