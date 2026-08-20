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
