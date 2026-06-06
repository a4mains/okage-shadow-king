"""Run pinning and execution-time verification, plus a driver round trip with a
synthetic native process (no okage-native, no owned assets)."""
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_native_route_support as support  # noqa: E402
from native_route import pins  # noqa: E402

PKG = 'tools/native_route'

def cli(script, *args, timeout=120):
    return subprocess.run([sys.executable, str(script)] + [str(a) for a in args], capture_output=True, text=True,
                          timeout=timeout)

def wait_for(predicate, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if predicate():
            return True
        time.sleep(0.05)
    return False

class PinFixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        self.stage, self.owned = support.make_stage(self.root / 'stage')
        self.prepared = support.make_prepared(self.root / 'prepared')
        self.saves = support.make_saves(self.root / 'saves')
        self.binary = support.make_fake_native(self.root / 'fake-okage-native')
        self.runs = self.root / 'runs'
        self.runs.mkdir()
        self.procs = []

    def tearDown(self):
        for run in self.runs.iterdir():
            proc = run / 'process.json'
            if proc.is_file():
                for key in ('native_pid', 'driver_pid'):
                    try:
                        os.kill(json.loads(proc.read_text())[key], 9)
                    except OSError:
                        pass
        self.tmp.cleanup()

    def prep_args(self, run, *extra):
        return ['prepare', '--run', run, '--binary', self.binary, '--stage', self.stage, '--prepared', self.prepared,
                '--saves', self.saves, '--prelude-ticks', 6, '--min-free-gib', 0] + list(extra)

    def prepare(self, name='r1', *extra):
        run = self.runs / name
        r = cli(support.CLI, *self.prep_args(run, *extra))
        self.assertEqual(r.returncode, 0, r.stderr)
        return run

    def pinned_cli(self, run):
        return run / PKG / 'cli.py'

class PrepareTest(PinFixture):
    def test_manifest_pins_everything(self):
        run = self.prepare()
        m = json.loads((run / 'manifest.json').read_text())
        self.assertEqual(m['historical_prefix_count'], 3)  # derived from the tape, not a default
        self.assertEqual((m['extension_count'], m['prepared_requests'], m['prepared_endpoint']), (2, 5, 11))
        for rel in ('okage-native', 'historical-requests.txt', 'extension-requests.txt', 'inputs/buttons.txt',
                    'inputs/directions.txt'):
            self.assertEqual(m['artifacts'][rel], pins.sha256(run / rel))
        package = sorted(p.name for p in (support.TOOLS / 'native_route').iterdir() if p.suffix in ('.py', '.md'))
        self.assertEqual(sorted(k.split('/')[-1] for k in m['artifacts'] if k.startswith(PKG)), package)
        self.assertEqual(set(m['initial_saves']), {'bkmo0.dat', 'bkmo1.dat', 'bkmo2.dat'})
        st = m['game_data_stage']
        self.assertEqual(st['resolved_path'], str((self.root / 'stage' / 'stage-real').resolve()))
        self.assertEqual(st['manifest_sha256'], pins.sha256(self.stage / '.owned-game-data-manifest.json'))
        ui = {e['path']: e for e in st['native_ui']}
        self.assertEqual(ui['battle-ui.bin']['target'], str((self.owned / 'battle-ui.bin').resolve()))
        self.assertIn('NATIVE_UI/battle-ui.bin', [e['path'] for e in st['top_two_levels']])
        self.assertEqual(m['python']['version'], __import__('platform').python_version())
        self.assertNotIn('--newgame-report', m['native_command'])
        self.assertIn(st['resolved_path'], m['native_command'])
        self.assertFalse((run / PKG / '__pycache__').exists())

    def test_newgame_report_requires_explicit_flag(self):
        run = self.prepare('r2', '--newgame-report')
        m = json.loads((run / 'manifest.json').read_text())
        self.assertTrue(m['newgame_report'])
        self.assertIn('--newgame-report', m['native_command'])

    def test_prepare_refusals(self):
        run = self.prepare()
        self.assertEqual(cli(support.CLI, *self.prep_args(run)).returncode, 10)  # existing run
        args = self.prep_args(self.runs / 'disk')
        args[args.index('--min-free-gib') + 1] = 10 ** 9
        r = cli(support.CLI, *args)
        self.assertEqual(r.returncode, 10)
        self.assertIn('GiB free', r.stderr)
        self.assertFalse((self.runs / 'disk').exists())
        (self.prepared / 'extension-requests.txt').write_text('step 2 0040\nseed 5\n')
        r = cli(support.CLI, *self.prep_args(self.runs / 'bad'))
        self.assertEqual(r.returncode, 10)
        self.assertIn('not an ordinary pad/observation request', r.stderr)

    def test_missing_required_arguments_are_usage_errors(self):
        r = cli(support.CLI, 'prepare', '--run', self.runs / 'x')
        self.assertEqual(r.returncode, 64)  # never 2 (2 means prelude-diverged)

    def test_run_scoped_commands_refuse_the_checkout_copy(self):
        run = self.prepare()
        r = cli(support.CLI, 'verify', run)
        self.assertEqual(r.returncode, 10)
        self.assertIn('must execute', r.stderr)

    def test_unlaunched_run_refuses_route_commands(self):
        run = self.prepare()
        r = cli(self.pinned_cli(run), 'verify', run)
        self.assertEqual(r.returncode, 10)
        self.assertIn('process.json missing', r.stderr)

class LaunchVerificationTest(PinFixture):
    def assert_launch_refused(self, run, needle):
        r = cli(self.pinned_cli(run), 'launch', run)
        self.assertEqual(r.returncode, 10, r.stderr)
        self.assertIn(needle, r.stderr)
        self.assertFalse((run / 'stdout.log').exists())
        self.assertFalse((run / 'process.json').exists())

    def test_changed_tool_refuses(self):
        run = self.prepare()
        with open(run / PKG / 'route.py', 'a') as f:
            f.write('\n# edited\n')
        self.assert_launch_refused(run, 'changed pinned tool route.py')

    def test_extra_tool_file_refuses(self):
        run = self.prepare()
        (run / PKG / 'extra.py').write_text('')
        self.assert_launch_refused(run, 'package files')

    def test_bytecode_cache_refuses(self):
        run = self.prepare()
        (run / PKG / '__pycache__').mkdir()
        self.assert_launch_refused(run, 'package files')

    def test_changed_tape_refuses(self):
        run = self.prepare()
        (run / 'historical-requests.txt').write_text('step 3 1000\nstatus\nview hist-b\n')
        self.assert_launch_refused(run, 'changed pinned artifact historical-requests.txt')

    def test_changed_prelude_input_refuses(self):
        run = self.prepare()
        (run / 'inputs' / 'buttons.txt').write_text('1 0040\n')
        self.assert_launch_refused(run, 'changed pinned artifact inputs/buttons.txt')

    def test_changed_binary_refuses(self):
        run = self.prepare()
        with open(run / 'okage-native', 'a') as f:
            f.write('\n')
        self.assert_launch_refused(run, 'changed pinned artifact okage-native')

    def test_changed_initial_save_refuses(self):
        run = self.prepare()
        (run / 'saves' / 'bkmo1.dat').write_bytes(support.save_bytes(8))
        self.assert_launch_refused(run, 'changed initial save bkmo1.dat')

    def test_changed_native_ui_target_refuses(self):
        run = self.prepare()
        (self.owned / 'battle-ui.bin').write_bytes(b'ui-v2')
        self.assert_launch_refused(run, 'game-data stage identity changed')

    def test_retargeted_stage_symlink_refuses(self):
        run = self.prepare()
        import shutil
        copy = self.root / 'stage' / 'stage-copy'
        shutil.copytree(self.root / 'stage' / 'stage-real', copy, symlinks=True)
        self.stage.unlink()
        os.symlink(copy, self.stage)
        self.assert_launch_refused(run, 'resolved_path')

    def test_added_native_ui_file_refuses(self):
        run = self.prepare()
        (self.root / 'stage' / 'stage-real' / 'NATIVE_UI' / 'new.bin').write_bytes(b'n')
        self.assert_launch_refused(run, 'native_ui')

    def test_changed_stage_manifest_refuses(self):
        run = self.prepare()
        (self.stage / '.owned-game-data-manifest.json').write_text('{"file_count": 2}\n')
        self.assert_launch_refused(run, 'manifest_sha256')

    def test_python_version_mismatch_refuses(self):
        run = self.prepare()
        m = json.loads((run / 'manifest.json').read_text())
        m['python']['version'] = '2.7.18'
        (run / 'manifest.json').write_text(json.dumps(m))
        self.assert_launch_refused(run, 'python')

    def test_historical_count_must_match_tape(self):
        run = self.prepare()
        m = json.loads((run / 'manifest.json').read_text())
        m['historical_prefix_count'] = 294
        (run / 'manifest.json').write_text(json.dumps(m))
        self.assert_launch_refused(run, 'historical_prefix_count')

class DriverRoundTripTest(PinFixture):
    def launch(self, name):
        run = self.runs / name
        r = cli(support.CLI, 'launch-new', *self.prep_args(run)[1:])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(wait_for(lambda: 'DRIVER_PREPARED_REQUESTS_COMPLETE' in (run / 'stdout.log').read_text()))
        return run

    def finish(self, run):
        r = cli(self.pinned_cli(run), 'send', run, 'quit')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(wait_for(lambda: (run / 'result.json').is_file()))
        return json.loads((run / 'result.json').read_text())

    def test_round_trip_journal_rejection_and_comparison(self):
        a = self.launch('a')
        fwd = [json.loads(l) for l in (a / 'forwarded-requests.jsonl').read_text().splitlines()]
        self.assertEqual([f['origin'] for f in fwd], ['prepared-historical-prefix'] * 3 + ['prepared-retained-extension'] * 2)
        r = cli(self.pinned_cli(a), 'send', a, 'step 4 2000', 'status')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('"frame":15', r.stdout)
        r = cli(self.pinned_cli(a), 'send', a, 'poke 0x1234 1')
        self.assertEqual(r.returncode, 10)  # refused before it reaches commands.in
        with open(a / 'commands.in', 'a') as f:
            f.write('poke 0x1234 1\nview hist-a\n')
        self.assertTrue(wait_for(lambda: json.loads((a / 'driver-state.json').read_text())['rejected'] == 2))
        res = self.finish(a)
        self.assertEqual((res['exit_code'], res['rejected'], res['prepared_complete']), (0, 2, True))
        rejected = [json.loads(l) for l in (a / 'forwarded-requests.jsonl').read_text().splitlines()
                    if json.loads(l)['origin'] == 'rejected']
        self.assertEqual([x['command'] for x in rejected], ['poke 0x1234 1', 'view hist-a'])

        b = self.launch('b')
        self.assertEqual(cli(self.pinned_cli(b), 'send', b, 'step 4 2000', 'status').returncode, 0)
        self.finish(b)
        r = cli(support.CLI, 'compare', a, b)
        self.assertEqual(r.returncode, 0, r.stdout[-2000:])
        self.assertIn('NOT image, audio or media equivalence', r.stdout)

        c = self.launch('c')
        self.assertEqual(cli(self.pinned_cli(c), 'send', c, 'step 4 1000', 'status').returncode, 0)
        self.finish(c)
        r = cli(support.CLI, 'compare', a, c)
        self.assertEqual(r.returncode, 1)
        report = json.loads(r.stdout)
        self.assertEqual(report['inputs']['comparison']['first_divergence']['frame'], {'a': 12, 'b': 12})

    def test_tool_change_after_launch_refuses_route_commands(self):
        run = self.launch('t')
        self.assertEqual(cli(self.pinned_cli(run), 'verify', run).returncode, 0)
        with open(run / PKG / 'battle.py', 'a') as f:
            f.write('\n')
        r = cli(self.pinned_cli(run), 'strict-full', run, '--target-save', 'bkmo2.dat')
        self.assertEqual(r.returncode, 10)
        self.assertFalse((run / 'route-result.json').exists())
        self.assertTrue((run / 'refusals.jsonl').is_file())

    def test_manifest_change_after_launch_refuses(self):
        run = self.launch('m')
        m = json.loads((run / 'manifest.json').read_text())
        m['label'] = 'edited'
        (run / 'manifest.json').write_text(json.dumps(m, indent=1) + '\n')
        r = cli(self.pinned_cli(run), 'verify', run)
        self.assertEqual(r.returncode, 10)
        self.assertIn('manifest.json changed after launch', r.stderr)

if __name__ == '__main__':
    unittest.main()
