"""Explicit prelude-candidate selection: every candidate retained, every
rejection recorded with the fixed rule, only a verified PASS selects."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_native_route_support  # noqa: E402,F401
from native_route import candidates, outcome, pins  # noqa: E402

def result(code, stage=None, reason='synthetic'):
    if code == 0:
        return {'status': 'PASS', 'exit_code': 0, 'stages': []}
    return {'status': 'FAIL', 'exit_code': code, 'failure': {'stage': stage, 'code': code, 'reason': reason}}

class CandidateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.parent = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def search(self, outcomes, max_n=5):
        """outcomes: list of (exit_code, route-result dict or None) per candidate."""
        queue = list(outcomes)
        launched = []

        def fake_launch(run, label=None, **kw):
            Path(run).mkdir()
            launched.append(label)
            return {'native_pid': 1, 'driver_pid': 1}

        def fake_run(cmd, stdout=None, stderr=None, cwd=None):
            code, res = queue.pop(0)
            if res is not None:
                (Path(cwd) / 'route-result.json').write_text(json.dumps(res))
            return subprocess.CompletedProcess(cmd, code)

        with mock.patch.object(candidates, 'prepare_and_launch', fake_launch), \
                mock.patch.object(candidates.subprocess, 'run', fake_run):
            code = candidates.run_candidates(self.parent, 'cand', max_n, 'bkmo2.dat', {'prelude_ticks': 21644})
        record = json.loads((self.parent / 'cand-candidates.json').read_text())
        return code, record, launched

    def test_divergences_are_recorded_then_pass_selected(self):
        div = (2, result(2, 'prelude-diverged', 'prelude diverged: queued scene=73 entrance=2 frame=2394'))
        code, rec, launched = self.search([div, div, (0, result(0))])
        self.assertEqual(code, 0)
        self.assertEqual(rec['selected'], 'candc')
        self.assertEqual(rec['selection_rule'], candidates.SELECTION_RULE)
        self.assertEqual([c['disposition'].split(':')[0] for c in rec['candidates']], ['rejected', 'rejected', 'selected'])
        self.assertIn('2394', rec['candidates'][0]['disposition'])
        for name in ('canda', 'candb', 'candc'):
            self.assertTrue((self.parent / name).is_dir())  # nothing deleted

    def test_later_stage_failure_ends_search_without_selection(self):
        div = (2, result(2, 'prelude-diverged'))
        code, rec, launched = self.search([div, (70, result(70, 'save', 'Save postconditions failed'))])
        self.assertEqual(code, 1)
        self.assertIsNone(rec['selected'])
        self.assertEqual(rec['outcome'], 'failed')
        self.assertIn('stage save code 70', rec['candidates'][1]['disposition'])
        self.assertEqual(launched, ['canda', 'candb'])

    def test_exit_2_without_divergence_record_is_not_a_divergence(self):
        code, rec, launched = self.search([(2, None), (0, result(0))])
        self.assertEqual(code, 1)
        self.assertEqual(launched, ['canda'])
        self.assertIn('without route-result', rec['candidates'][0]['disposition'])

    def test_exit_0_with_failed_result_is_not_selected(self):
        code, rec, _ = self.search([(0, result(50, 'home'))])
        self.assertEqual(code, 1)
        self.assertIsNone(rec['selected'])
        self.assertIn('disagrees', rec['candidates'][0]['disposition'])

    def test_exhausted_candidates(self):
        div = (2, result(2, 'prelude-diverged'))
        code, rec, launched = self.search([div, div], max_n=2)
        self.assertEqual(code, 1)
        self.assertTrue(rec['outcome'].startswith('exhausted'))
        self.assertEqual(len(rec['candidates']), 2)

    def test_launch_refusal_is_recorded(self):
        def refuse(run, label=None, **kw):
            raise pins.PinError(['refusing: 10 GiB free < 200 GiB'])
        with mock.patch.object(candidates, 'prepare_and_launch', refuse):
            code = candidates.run_candidates(self.parent, 'cand', 3, 'bkmo2.dat', {})
        rec = json.loads((self.parent / 'cand-candidates.json').read_text())
        self.assertEqual(code, 1)
        self.assertIn('not launched', rec['candidates'][0]['disposition'])

    def test_existing_record_is_never_overwritten(self):
        (self.parent / 'cand-candidates.json').write_text('{}')
        with self.assertRaises(pins.PinError):
            candidates.run_candidates(self.parent, 'cand', 1, 'bkmo2.dat', {})
        self.assertEqual((self.parent / 'cand-candidates.json').read_text(), '{}')

    def test_divergence_code_is_reserved(self):
        self.assertEqual(outcome.CODES['prelude-diverged'], 2)
        self.assertEqual(len(set(outcome.CODES.values())), len(outcome.CODES))

if __name__ == '__main__':
    unittest.main()
