"""Development prefix (prefix_mode "dev"): tape re-timing at the bedroom
activation, pin refusal, failure code and strict-full recording. Synthetic only."""
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
from native_route import dev_route, outcome, pins, route, session_driver, strict_full  # noqa: E402
from test_native_route_stages import FakeCtl, failing, field_status, passing  # noqa: E402

BUTTONS = [(1, 0), (809, 0x40), (830, 0), (2591, 0x2000), (3000, 0)]
DIRECTIONS = [(1, 0), (2000, 0x1000), (2100, 0)]
RETAINED_HIST = ['step 5 0000', 'status', 'view x', 'observe']
RETAINED_EXT = ['step 7 0040', 'status']

def write_dev(run, buttons=BUTTONS, directions=DIRECTIONS):
    d = Path(run) / pins.DEV_DIR
    d.mkdir(parents=True, exist_ok=True)
    (d / pins.DEV_TAPES[0]).write_text(''.join(f'{f} {m:04x}\n' for f, m in buttons))
    (d / pins.DEV_TAPES[1]).write_text(''.join(f'{f} {m:04x}\n' for f, m in directions))
    (d / pins.DEV_TAPES[2]).write_text('\n'.join(RETAINED_HIST) + '\n')
    (d / pins.DEV_TAPES[3]).write_text('\n'.join(RETAINED_EXT) + '\n')
    arts = {pins.DEV_DIR + '/' + n: pins.sha256(d / n) for n in pins.DEV_TAPES}
    return {'dev_tapes': list(pins.DEV_TAPES), 'artifacts': arts}

class DevCtl(FakeCtl):
    """Frame-tracking controller: scene 74 once frame >= activation, scene 1
    once any retained request has been sent."""

    def __init__(self, activation, start=808, road=True):
        super().__init__()
        self.frame, self.activation, self.road, self.requests = start, activation, road, []
        self.log = f'Native frames=808 \n{route.PREPARED_COMPLETE}\n'

    def step(self, n, mask=0):
        self.sent.append(f'step {n} {mask:04x}')
        self.frame += n

    def send(self, *cmds):
        self.requests.extend(cmds)
        self.sent.extend(cmds)
        return []

    def status(self):
        self.sent.append('status')
        scene = 1 if (self.requests and self.road) else 74 if self.frame >= self.activation else 0
        return field_status(self.frame, scene=scene)

class AlignmentTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.run = Path(self.tmp.name)
        self.manifest = write_dev(self.run)

    def tearDown(self):
        self.tmp.cleanup()

    def test_mask_is_union_of_both_tapes(self):
        mask_at, frames = dev_route.load_tape(self.run)
        self.assertEqual(frames, [1, 809, 830, 2000, 2100, 2591, 3000])
        self.assertEqual([mask_at(f) for f in (808, 809, 830, 2000, 2100, 2591, 3000)],
                         [0, 0x40, 0, 0x1000, 0, 0x2000, 0])

    def test_activation_inside_the_pre_activation_tape(self):
        c = DevCtl(activation=900)
        ev = dev_route.prefix(c, self.run, self.manifest)
        expected = [
            'status',  # frame 808
            'step 1 0000', 'status', 'step 20 0040', 'status',  # 808->809, 809->829
            'step 1 0040', 'status',  # 829->830
            'step 20 0000', 'status', 'step 20 0000', 'status', 'step 20 0000', 'status', 'step 20 0000', 'status',
            'status',
            'status',  # A
            # aligned replay of tape 2591.. at A: held 0x2000 until 3000, then 0
            'step 409 2000', 'step 6000 0000', 'step 6000 0000', 'step 6000 0000', 'step 644 0000',
            'status',
        ]
        self.assertEqual(c.sent[:len(expected)], expected)
        self.assertEqual(c.sent[len(expected):],
                         ['step 5 0000', 'status', 'step 7 0040', 'status', 'status'])
        self.assertEqual(c.requests, ['step 5 0000', 'status', 'step 7 0040', 'status'])  # view/observe not re-sent
        self.assertEqual((ev['prefix_mode'], ev['activation_frame'], ev['alignment_offset']), ('dev', 910, 910 - 2591))
        self.assertEqual(ev['tape_frame_reached_at_activation'], 910)

    def test_activation_after_the_tape_window_steps_one_tick(self):
        c = DevCtl(activation=2600)
        ev = dev_route.prefix(c, self.run, self.manifest)
        steps = [s for s in c.sent if s.startswith('step')]
        self.assertEqual(steps.count('step 1 0000'), 1 + 9 + 0)  # 808->809 plus the 9 ticks 2591->2600
        self.assertEqual(ev['activation_frame'], 2600)
        self.assertEqual(ev['tape_frame_reached_at_activation'], 2600)
        self.assertEqual(ev['alignment_offset'], 9)
        # the replayed tail starts at the held mask of tape frame 2591
        self.assertIn('step 409 2000', steps)

    def test_replay_without_until_uses_big_chunks_only(self):
        c = DevCtl(activation=0)
        mask_at, frames = dev_route.load_tape(self.run)
        f = dev_route.replay(c, mask_at, frames, 2591, 21644)
        self.assertEqual(f, 21644)
        self.assertEqual(sum(int(s.split()[1]) for s in c.sent if s.startswith('step')), 21644 - 2591)
        self.assertNotIn('status', c.sent)

class FailureTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.run = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_refuses_when_dev_tapes_not_pinned(self):
        c = DevCtl(activation=900)
        with self.assertRaises(outcome.StageFailure) as e:
            dev_route.prefix(c, self.run, {'artifacts': {}, 'dev_tapes': []})
        self.assertEqual((e.exception.stage, e.exception.code), ('pins', 10))
        self.assertEqual(c.sent, [])
        self.assertTrue(dev_route.pinned_problems(self.run, {'artifacts': {}}))

    def test_refuses_when_pinned_dev_tape_changed(self):
        m = write_dev(self.run)
        (self.run / pins.DEV_DIR / pins.DEV_TAPES[2]).write_text('status\n')
        problems = dev_route.pinned_problems(self.run, m)
        self.assertTrue(any('changed pinned dev tape' in p for p in problems))
        with self.assertRaises(outcome.StageFailure) as e:
            dev_route.prefix(DevCtl(900), self.run, m)
        self.assertEqual(e.exception.code, 10)

    def test_not_on_the_road_fails_with_own_code(self):
        m = write_dev(self.run)
        with self.assertRaises(outcome.StageFailure) as e:
            outcome.run_stage(dev_route.STAGE, dev_route.prefix, DevCtl(900, road=False), self.run, m)
        self.assertEqual((e.exception.stage, e.exception.code), ('route-dev-prefix', 23))
        self.assertIn('not on the road', e.exception.reason)

    def test_status_frame_must_be_808(self):
        m = write_dev(self.run)
        with self.assertRaises(outcome.StageFailure) as e:
            dev_route.prefix(DevCtl(900, start=809), self.run, m)
        self.assertEqual(e.exception.code, 23)

    def test_route_main_dev_mode_records_stage_and_continues(self):
        m = write_dev(self.run)
        stages = []
        with mock.patch.object(route, 'walk', return_value={'w': 1}), \
                mock.patch.object(route, 'rosalyn', return_value={'r': 1}), \
                mock.patch.object(route.encounter, 'main', return_value={'e': 1}):
            route.main(DevCtl(900), self.run, m, 'contact', lambda n, ev: stages.append(n), prefix_mode='dev')
        self.assertEqual(stages, ['route-dev-prefix', 'route-walk', 'route-rosalyn', 'encounter'])

class PrepareDevTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        self.stage, _ = support.make_stage(self.root / 'stage')
        self.prepared = support.make_prepared(self.root / 'prepared', historical=[], extension=[])
        self.saves = support.make_saves(self.root / 'saves')
        self.binary = support.make_fake_native(self.root / 'fake')

    def tearDown(self):
        self.tmp.cleanup()

    def prepare(self, name):
        return session_driver.prepare(self.root / name, self.binary, self.stage, self.prepared, self.saves, 6,
                                      min_free_gib=0)

    def test_dev_tapes_are_pinned_when_present(self):
        write_dev(self.prepared)
        for n in pins.DEV_TAPES:
            (self.prepared / n).write_text((self.prepared / pins.DEV_DIR / n).read_text())
        m = self.prepare('r1')
        run = Path(m['run'])
        self.assertEqual(m['dev_tapes'], list(pins.DEV_TAPES))
        for n in pins.DEV_TAPES:
            self.assertEqual(m['artifacts']['dev/' + n], pins.sha256(run / 'dev' / n))
        self.assertEqual((m['historical_prefix_count'], m['prepared_requests']), (0, 0))
        self.assertEqual(dev_route.pinned_problems(run, m), [])
        (run / 'dev' / pins.DEV_TAPES[2]).write_text('status\n')
        self.assertTrue(dev_route.pinned_problems(run, m))

    def test_absent_dev_tapes_are_not_pinned(self):
        m = self.prepare('r2')
        self.assertEqual(m['dev_tapes'], [])
        self.assertFalse((Path(m['run']) / 'dev').exists())
        self.assertTrue(dev_route.pinned_problems(m['run'], m))

    def test_partial_dev_tapes_refuse_prepare(self):
        (self.prepared / pins.DEV_TAPES[0]).write_text('1 0000\n')
        with self.assertRaises(pins.PinError):
            self.prepare('r3')

class StrictFullPrefixModeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.run = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def go(self, **kw):
        seen = {}

        def route_fn(*a, **k):
            seen.update(k)
            return passing('encounter')(*a, **k)
        with mock.patch.object(strict_full.route, 'main', route_fn), \
                mock.patch.object(strict_full.battle, 'fight_if_needed', lambda *a: {'v': 1}), \
                mock.patch.object(strict_full.home, 'main', lambda *a: {'h': 1}), \
                mock.patch.object(strict_full.home_inside, 'main', passing('save')), \
                contextlib.redirect_stdout(io.StringIO()):
            code = strict_full.run_route(object(), self.run, {'_sha256': 'x'}, 'bkmo2.dat', **kw)
        return code, seen, json.loads((self.run / 'route-result.json').read_text())

    def test_default_is_historical_and_recorded(self):
        code, seen, r = self.go()
        self.assertEqual((code, r['prefix_mode'], seen['prefix_mode']), (0, 'historical', 'historical'))

    def test_dev_is_recorded_and_passed(self):
        code, seen, r = self.go(prefix_mode='dev')
        self.assertEqual((code, r['prefix_mode'], seen['prefix_mode']), (0, 'dev', 'dev'))

    def test_failure_still_records_mode(self):
        with mock.patch.object(strict_full.route, 'main', failing('route-dev-prefix')), \
                contextlib.redirect_stdout(io.StringIO()):
            code = strict_full.run_route(object(), self.run, {'_sha256': 'x'}, 'bkmo2.dat', prefix_mode='dev')
        r = json.loads((self.run / 'route-result.json').read_text())
        self.assertEqual((code, r['prefix_mode'], r['failure']['stage']), (23, 'dev', 'route-dev-prefix'))

if __name__ == '__main__':
    unittest.main()

class DevRosalynSegmentTests(unittest.TestCase):
    def test_dev_mode_reads_the_pinned_dev_request_tape(self):
        StageFailure = outcome.StageFailure
        with tempfile.TemporaryDirectory() as d:
            run = Path(d)
            (run / pins.DEV_DIR).mkdir()
            tape = ['step 4 0040'] * 300
            (run / pins.DEV_DIR / pins.DEV_REQUEST_TAPES[0]).write_text('\n'.join(tape) + '\n')
            (run / pins.TAPES[0]).write_text('')
            name = pins.DEV_DIR + '/' + pins.DEV_REQUEST_TAPES[0]
            manifest = {'artifacts': {name: 'x'}, 'historical_prefix_count': 0}
            self.assertEqual(len(route.rosalyn_segment(run, manifest, 'dev')), 195)
            with self.assertRaises(StageFailure):  # the empty RUN/historical-requests.txt
                route.rosalyn_segment(run, manifest, 'historical')
            with self.assertRaises(StageFailure):  # dev tape not pinned
                route.rosalyn_segment(run, {'artifacts': {}, 'historical_prefix_count': 0}, 'dev')
