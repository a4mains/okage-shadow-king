"""madril route-card harness: fixed endpoint postconditions, each with a
negative control that must fail (never a false pass)."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_native_route_support as support  # noqa: E402,F401
from native_route import madril, outcome  # noqa: E402

def variables(story=7, scene=14, objective=11, flag125=1):
    v = bytearray(0x1000)
    v[0x210] = story & 0xff
    struct.pack_into('<i', v, 0x510, scene)
    struct.pack_into('<i', v, 0x538, objective)
    if flag125:
        v[0x110 + 125 // 8] |= 1 << (7 - 125 % 8)
    return bytes(v)

def status(frame, scene=14, event=False, message=0, pos=(0.0, 0.0, 0.0)):
    return {'frame': frame, 'scene': scene, 'entrance': 1, 'event_running': event, 'pc': 0, 'message': message,
            'choice': 0, 'camera_yaw': 0.0, 'hero_position': list(pos), 'guard_pending_opcode': 0}

class Ctl:
    def __init__(self, statuses, log='', run=None, capture=None):
        self.statuses = list(statuses)
        self.i = 0
        self.log = log
        self.run = run
        self.capture = capture
        self.sent = []

    def status(self):
        return self.statuses[min(self.i, len(self.statuses) - 1)]

    def step(self, n, mask=0):
        self.sent.append((n, mask))
        self.i += 1

    def advance(self, until, confirm_gap=40, max_ticks=600, hold=4):
        s = self.status()
        while not until(s) and self.i < len(self.statuses) - 1:
            self.i += 1
            s = self.status()
        return s

    def log_offset(self):
        return 0

    def log_since(self, offset):
        return self.log

    def send(self, *cmds):
        self.sent.extend(cmds)
        if self.capture is not None and any(c.startswith('capture madril-arrival') for c in cmds):
            p = Path(self.run) / 'control' / 'madril-arrival.png.variables.bin'
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(self.capture)
        return []

    def alive(self):
        return True

    def observe(self):
        return {'battle': False}

class ProgressTests(unittest.TestCase):
    def test_progress_words(self):
        self.assertEqual(madril.progress(variables()), {'story': 7, 'scene_word': 14, 'objective': 11, 'b1_125': 1})
        self.assertEqual(madril.progress(variables(objective=10, flag125=0))['b1_125'], 0)

    def test_guard_lines_ignore_status_json(self):
        log = '{"guard_pending_opcode":0}\nHeadless ready frame=3\nBattle music variant ownership is unrecovered: 1\n'
        self.assertEqual(madril.guard_lines(log), ['Battle music variant ownership is unrecovered: 1'])
        self.assertEqual(madril.guard_lines('Unsupported map effect primitive\nUnreviewed music resource ID 3\n'),
                         ['Unsupported map effect primitive', 'Unreviewed music resource ID 3'])

class MessageTests(unittest.TestCase):
    def test_requires_the_arrival_message(self):
        c = Ctl([status(1, event=True, message=21001), status(2, event=False, message=21001)])
        with self.assertRaises(outcome.StageFailure) as e:
            madril.through_message(c, 0)
        self.assertEqual(e.exception.stage, 'madril-message')

    def test_message_seen_and_event_finished(self):
        c = Ctl([status(1, event=True, message=21020), status(5, event=True, message=21020),
                 status(9, event=False, message=21020)])
        r = madril.through_message(c, 0)
        self.assertEqual(r['message_frames'], [1, 9])
        self.assertFalse(r['after']['event_running'])

    def test_event_still_running_fails(self):
        c = Ctl([status(1, event=True, message=21020)] * 3)
        with self.assertRaises(outcome.StageFailure):
            madril.through_message(c, 0)

class MainTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.run = Path(self.tmp.name)
        restored = variables(scene=74, objective=10, flag125=0)
        (self.run / 'control').mkdir()
        (self.run / 'control' / 'restored-field.png.variables.bin').write_bytes(restored)
        self.patches = [
            mock.patch.object(madril.restore_check, 'check_slot', lambda *a: restored[:0xd78]),
            mock.patch.object(madril.restore_check, 'payload_differences', lambda *a: []),
            mock.patch.object(madril.restore_route, 'main', lambda *a: {'restored': 1}),
            mock.patch.object(madril.restore_check, 'leave_house', lambda *a: ({}, {}, {})),
            mock.patch.object(madril.restore_check, 'to_patrol', lambda *a: None),
            mock.patch.object(madril, 'walk_road', lambda c, run, record: record('road', {})),
            mock.patch.object(madril, 'confirm_trigger', lambda *a: 0),
            mock.patch.object(madril, 'arrive', lambda *a: status(10)),
            mock.patch.object(madril, 'through_message', lambda *a: {'message_frames': [11, 20]}),
            mock.patch.object(madril, 'in_field_control', lambda s, scene=None: True),
        ]
        for p in self.patches:
            p.start()

    def tearDown(self):
        for p in self.patches:
            p.stop()
        self.tmp.cleanup()

    def go(self, capture, log='', moves=True):
        pos = [(0.0, 0.0, 0.0), (0.0, 0.0, 3.0) if moves else (0.0, 0.0, 0.0)]
        c = Ctl([status(30, pos=pos[0])] * 4 + [status(60, pos=pos[1])] * 20, log=log, run=self.run, capture=capture)
        stages = []
        madril.main(c, self.run, {}, 'bkmo2.dat', 2, 'ab' * 32, lambda n, e: stages.append(n), rest_first=False)
        return stages

    def test_pass(self):
        self.assertEqual(self.go(variables()), ['slot', 'restore-route', 'payload', 'transitions', 'road', 'arrival',
                                                'message', 'state', 'control'])

    def test_objective_not_advanced_fails(self):
        with self.assertRaises(outcome.StageFailure) as e:
            self.go(variables(objective=10))
        self.assertEqual(e.exception.stage, 'madril-state')

    def test_flag_not_set_fails(self):
        with self.assertRaises(outcome.StageFailure):
            self.go(variables(flag125=0))

    def test_guard_line_fails(self):
        with self.assertRaises(outcome.StageFailure) as e:
            self.go(variables(), log='Unrecovered field mode request 3\n')
        self.assertEqual(e.exception.stage, 'madril-control')

    def test_no_movement_fails(self):
        with self.assertRaises(outcome.StageFailure) as e:
            self.go(variables(), moves=False)
        self.assertEqual(e.exception.stage, 'madril-control')

    def test_wrong_entry_state_fails(self):
        (self.run / 'control' / 'restored-field.png.variables.bin').write_bytes(variables(scene=74, objective=61,
                                                                                            flag125=0))
        with self.assertRaises(outcome.StageFailure) as e:
            self.go(variables())
        self.assertEqual(e.exception.stage, 'restore-check')

if __name__ == '__main__':
    unittest.main()

class WalkRoadTests(unittest.TestCase):
    class Walker:
        def __init__(self, reach):
            self.reach = reach
            self.pos = (0.0, 0.0)
            self.walks = 0

        def status(self):
            return status(1, scene=1, pos=(self.pos[0], 0.0, self.pos[1]))

        def walk_to(self, x, z, tol=1.0, max_ticks=0, stop=None):
            self.walks += 1
            if self.reach(x, z):
                self.pos = (x, z)
            return self.status()

        def note(self, *a):
            pass

    def run_walk(self, reach, points):
        c = self.Walker(reach)
        stages = []
        with mock.patch.object(madril.madril_plan, 'WAYPOINTS', points), \
                mock.patch.object(madril.battle, 'fight_if_needed', lambda *a: False), \
                mock.patch.object(madril.navigate, 'navigate', lambda c, run, t, **k: c.walk_to(*t)):
            madril.walk_road(c, None, lambda n, e: stages.append(n))
        return c, stages

    def test_reaches_trigger(self):
        c, stages = self.run_walk(lambda x, z: True, [(0, 0), (10, 0), (20, 0)])
        self.assertEqual(stages, ['road'])

    def test_stall_fails_after_three_misses(self):
        with self.assertRaises(outcome.StageFailure) as e:
            self.run_walk(lambda x, z: x < 15, [(0, 0), (10, 0), (20, 0), (30, 0), (40, 0), (50, 0)])
        self.assertEqual(e.exception.stage, 'madril-road')
        self.assertIn('stalled', e.exception.reason)

    def test_isolated_miss_is_tolerated_but_trigger_must_be_reached(self):
        with self.assertRaises(outcome.StageFailure) as e:
            self.run_walk(lambda x, z: x != 10 and x < 25, [(0, 0), (10, 0), (20, 0)])
        self.assertIn('trigger', e.exception.reason)

class ConfirmMarkerTests(unittest.TestCase):
    def test_confirm_marker_requires_an_event(self):
        class C(WalkRoadTests.Walker):
            def __init__(self):
                super().__init__(lambda x, z: True)
                self.events = 0
            def step(self, n, mask=0):
                pass
            def roar_active(self):
                return False
            def observe(self):
                return {'battle': False}
            pick = staticmethod(lambda yaw, dx, dz, exclude=(): 0x1000)
        c = C()
        with mock.patch.object(madril.madril_plan, 'WAYPOINTS', [(0, 0), ('confirm', 0, -5, 3, 60), (10, 0)]), \
                mock.patch.object(madril.battle, 'fight_if_needed', lambda *a: False):
            with self.assertRaises(outcome.StageFailure) as e:
                madril.walk_road(c, None, lambda n, ev: None)
        self.assertIn('Confirm started no event', e.exception.reason)

class ConfirmEventContactTests(unittest.TestCase):
    def test_roar_before_the_talk_is_fought_first(self):
        class C:
            def __init__(self):
                self.roar, self.event, self.fought, self.confirms = True, False, 0, 0
            def status(self):
                return status(1, scene=1, event=self.event)
            def roar_active(self):
                return self.roar
            def observe(self):
                return {'battle': False}
            def step(self, n, mask=0):
                from native_route.pilot import CONFIRM
                if mask & CONFIRM:
                    self.confirms += 1
                    if not self.roar:
                        self.event = True
            def event_dialogue(self, stage):
                self.event = False
                return status(2, scene=1)
            def send(self, *cmds):
                return []
            pick = staticmethod(lambda yaw, dx, dz, exclude=(): 0x1000)
        c = C()

        def fight(ctl, run):
            if ctl.roar:
                ctl.roar = False
                ctl.fought += 1
                return {'won': True}
            return None
        with tempfile.TemporaryDirectory() as d, mock.patch.object(madril.battle, 'fight_if_needed', fight):
            r = madril.confirm_event(c, d, 0, (0, -5))
        self.assertEqual((c.fought, c.confirms), (1, 1))
        self.assertTrue(r['started']['event_running'])

class HotelSaveTests(unittest.TestCase):
    """madril_save / restore_madril postconditions: each negative must fail."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.run = Path(self.tmp.name)
        (self.run / 'saves').mkdir()
        (self.run / 'control').mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    class Menu:
        """Greeting page 0 turns on the Nth Confirm; then a CC menu (rows 0..2)
        that arms on the first Down; `stuck` keeps the cursor from moving."""
        def __init__(self, confirms_to_menu=1, stuck=False):
            self.page, self.needed, self.armed, self.cursor, self.stuck = 0, confirms_to_menu, False, 0, stuck
            self.confirms = 0
        def step(self, n, mask=0):
            from native_route.pilot import CONFIRM, DOWN
            if self.page == 0 and mask & CONFIRM:
                self.confirms += 1
                if self.confirms > self.needed:
                    self.page = 1
            elif self.page == 1 and mask & DOWN:
                if self.armed and not self.stuck:
                    self.cursor = min(self.cursor + 1, 2)
                self.armed = True
            if self.page == 1 and mask & CONFIRM and self.cursor == 2 and self.armed:
                self.thanks += 1
                if self.thanks == 2:
                    self.log += 'Native field Save constructed frame=1\n'
                if self.thanks == 5:
                    self.log += 'Native field Save retired frame=2\n'
        thanks = 0
        log = ''
        def log_offset(self):
            return len(self.log)
        def log_since(self, offset):
            return self.log[offset:]
        def send(self, *cmds):
            return []
        def observe(self):
            return {'message': 30125, 'text_progress': self.page, 'choice': self.cursor, 'battle': False}
        def status(self):
            return status(1, scene=107)

    def test_hotel_menu_waits_for_the_menu_page_and_reaches_save(self):
        from native_route import madril_save
        m = self.Menu(confirms_to_menu=3)       # slow greeting (c2p-final-v2a)
        after, moved = madril_save.save_inputs(m, 'x')
        self.assertEqual((m.page, after['scene']), (1, 107))

    def test_hotel_menu_cursor_must_be_on_save(self):
        from native_route import madril_save
        with self.assertRaises(outcome.StageFailure) as e:
            madril_save.save_inputs(self.Menu(stuck=True), 'x')
        self.assertIn('not on Save', e.exception.reason)

    def test_save_screen_must_be_constructed(self):
        from native_route import madril_save
        m = self.Menu()
        m.step = (lambda orig: (lambda n, mask=0: (orig(n, mask), setattr(m, 'log', ''))))(m.step)
        with self.assertRaises(outcome.StageFailure) as e:
            madril_save.save_inputs(m, 'x')
        self.assertIn('never constructed', e.exception.reason)

    def test_menu_page_must_be_reached(self):
        from native_route import madril_save
        with self.assertRaises(outcome.StageFailure) as e:
            madril_save.save_inputs(self.Menu(confirms_to_menu=99), 'x')
        self.assertIn('menu page not reached', e.exception.reason)

    def run_save(self, reasons, payload_state):
        from native_route import madril_save
        payload = variables(scene=107, objective=11, flag125=1, **payload_state)[:0xd78]
        (self.run / 'saves' / 'bkmo2.dat').write_bytes(b'!crc' + bytes(8) + payload)
        stages = []
        with mock.patch.object(madril_save, 'to_hotel', lambda c, run: status(1, scene=107)), \
                mock.patch.object(madril_save, 'save_inputs', lambda c, tag: (status(2, scene=107), status(3, scene=107))), \
                mock.patch.object(madril_save.home_inside, 'verify_save', lambda *a, **k: ({'sha256': 'x'}, reasons)), \
                mock.patch.object(madril_save.savefmt, 'payload', lambda data: data[12:]):
            madril_save.main(Ctl([status(1)]), self.run, {}, 'bkmo2.dat', lambda n, e: stages.append(n))
        return stages

    def test_save_passes_with_boundary_state(self):
        self.assertEqual(self.run_save([], {}), ['hotel', 'save'])

    def test_failed_save_postconditions_fail(self):
        with self.assertRaises(outcome.StageFailure) as e:
            self.run_save(['target save unchanged from its pinned initial hash'], {})
        self.assertEqual(e.exception.stage, 'save')

    def test_wrong_saved_state_fails(self):
        with self.assertRaises(outcome.StageFailure) as e:
            self.run_save([], {'story': 8})
        self.assertIn('boundary state', e.exception.reason)

    def test_restore_madril_requires_the_hotel_state(self):
        from native_route import restore_madril
        (self.run / 'control' / 'restored-field.png.variables.bin').write_bytes(
            variables(scene=107, objective=61, flag125=1))
        with mock.patch.object(restore_madril.restore_check, 'check_slot', lambda *a: b''), \
                mock.patch.object(restore_madril.restore_check, 'payload_differences', lambda *a: []), \
                mock.patch.object(restore_madril.restore_route, 'main', lambda *a: {}):
            with self.assertRaises(outcome.StageFailure) as e:
                restore_madril.main(Ctl([status(1, scene=107)]), self.run, {}, 'bkmo2.dat', 2, 'ab' * 32,
                                    lambda n, ev: None)
        self.assertIn('hotel Save state', e.exception.reason)
