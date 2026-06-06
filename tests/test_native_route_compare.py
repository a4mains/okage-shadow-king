"""Strict comparator: positive control and negative controls on synthetic runs."""
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_native_route_support as support  # noqa: E402
from native_route import compare as cmp  # noqa: E402

def find(lines, **want):
    for i, line in enumerate(lines):
        if line.startswith('{'):
            rec = json.loads(line)
            if all(rec.get(k) == v for k, v in want.items()):
                return i
    raise AssertionError('record not found: %r' % want)

class ComparatorTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.a = support.make_compare_run(root / 'a')
        self.b = support.make_compare_run(root / 'b')

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *extra):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = cmp.main([str(self.a), str(self.b)] + list(extra))
        return code, json.loads(out.getvalue())

    def edit(self, run, name, fn):
        path = Path(run) / name
        items = support.lines(path)
        fn(items)
        support.write_lines(path, items)

    # ---- positive control ----
    def test_identical_runs_pass_and_state_scope(self):
        code, r = self.run_cli()
        self.assertEqual(code, 0, r['failures'])
        self.assertEqual(r['verdict'], 'IDENTICAL_AND_FULLY_ACCOUNTED')
        self.assertIn('NOT image, audio or media equivalence', r['scope'])
        self.assertEqual(r['inputs']['a']['interval'], [1, 15])
        self.assertEqual(r['status']['a']['repeated_frames'], 1)  # two status rows at frame 9 kept
        self.assertEqual(r['inputs']['a']['kinds'], {'tick_attempt': 15, 'tick_complete': 15, 'pause': 4, 'resume': 3})

    # ---- negative controls ----
    def test_duplicate_attempt(self):
        def dup(items):
            i = find(items, kind='tick_attempt', frame=8)
            items.insert(i + 1, items[i])
        self.edit(self.b, 'inputs.jsonl', dup)
        code, r = self.run_cli()
        self.assertEqual(code, 1)
        self.assertEqual(r['inputs']['b']['duplicate_attempt_frames']['examples'], [8])
        self.assertIn('b.inputs.duplicate_attempt_frames', r['failures'])
        d = r['inputs']['comparison']['first_divergence']
        self.assertEqual(d['frame'], {'a': 8, 'b': 8})
        self.assertEqual(d['a']['record']['kind'], 'tick_complete')
        self.assertEqual(d['b']['record']['kind'], 'tick_attempt')
        self.assertEqual(r['inputs']['comparison']['multiplicity_differences']['examples'], [{'frame': 8, 'a': 2, 'b': 3}])

    def test_dropped_repeated_status_row(self):
        def drop(items):
            rows = [i for i, l in enumerate(items) if l.startswith('{"frame":9,')]
            self.assertEqual(len(rows), 2)
            del items[rows[1]]
        self.edit(self.b, 'stdout.log', drop)
        code, r = self.run_cli()
        self.assertEqual(code, 1)
        self.assertEqual(r['status']['b']['rows_missing_vs_requests']['examples'], [9])
        self.assertIn('b.status.status_rows_differ_from_status_requests', r['failures'])
        self.assertEqual(r['status']['comparison']['multiplicity_differences']['examples'], [{'frame': 9, 'a': 2, 'b': 1}])
        self.assertEqual(r['status']['comparison']['first_divergence']['index'], 1)

    def test_reordered_status_rows(self):
        def swap(items):
            i = next(k for k, l in enumerate(items) if l.startswith('{"frame":11,'))
            j = next(k for k, l in enumerate(items) if l.startswith('{"frame":15,'))
            items[i], items[j] = items[j], items[i]
        self.edit(self.b, 'stdout.log', swap)
        code, r = self.run_cli()
        self.assertEqual(code, 1)
        self.assertEqual(r['status']['b']['frame_order_violations']['count'], 1)
        self.assertEqual(r['status']['comparison']['first_divergence']['frame'], {'a': 11, 'b': 15})

    def test_reordered_input_records(self):
        def swap(items):
            i = find(items, kind='tick_attempt', frame=12)
            j = find(items, kind='tick_attempt', frame=13)
            items[i], items[j] = items[j], items[i]
        self.edit(self.b, 'inputs.jsonl', swap)
        code, r = self.run_cli()
        self.assertEqual(code, 1)
        self.assertGreater(r['inputs']['b']['frame_order_violations']['count'], 0)
        self.assertGreater(r['inputs']['b']['unresolved_attempts']['count'], 0)
        self.assertEqual(r['inputs']['comparison']['first_divergence']['frame'], {'a': 12, 'b': 13})

    def test_truncated_tail_default_interval(self):
        def cut(items):
            del items[find(items, kind='tick_attempt', frame=14):]
        self.edit(self.b, 'inputs.jsonl', cut)
        code, r = self.run_cli()
        self.assertEqual(code, 1)
        tail = r['inputs']['comparison']['tail_on_one_side']
        self.assertEqual(tail['ends_early'], 'b')
        self.assertEqual(tail['extra_frames'], [14, 15])
        self.assertEqual(r['inputs']['comparison']['frames_missing_in_b']['examples'], [[14, 15]])

    def test_truncated_tail_explicit_interval_is_unaccounted(self):
        def cut(items):
            del items[find(items, kind='tick_attempt', frame=14):]
        self.edit(self.b, 'inputs.jsonl', cut)
        code, r = self.run_cli('--a-inputs', '1:15', '--b-inputs', '1:15')
        self.assertEqual(code, 1)
        self.assertEqual(r['inputs']['b']['uncaptured_tail'], {'stream_last_attempt_frame': 13, 'interval_hi': 15})
        self.assertEqual(r['inputs']['b']['missing_attempt_frames']['examples'], [[14, 15]])
        self.assertIn('b.inputs.uncaptured_tail', r['failures'])

    def test_extra_tail(self):
        root = Path(self.tmp.name)
        self.b = support.make_compare_run(root / 'b2', requests=[
            'step 3 1000', 'status', 'status', 'step 2 0040', 'status', 'step 4 0000', 'status', 'step 2 0000', 'status'])
        code, r = self.run_cli()
        self.assertEqual(code, 1)
        self.assertTrue(r['inputs']['b']['fully_accounted'])  # B is self-consistent ...
        tail = r['inputs']['comparison']['tail_on_one_side']
        self.assertEqual(tail['ends_early'], 'a')  # ... but has records A lacks
        self.assertEqual(tail['extra_frames'], [15, 17])
        self.assertEqual(r['status']['comparison']['tail_on_one_side']['ends_early'], 'a')

    def test_differing_pressed_mask(self):
        def flip(items):
            i = find(items, kind='tick_attempt', frame=10)
            rec = json.loads(items[i])
            rec['pressed'] = 0
            items[i] = json.dumps(rec, separators=(',', ':'))
        self.edit(self.b, 'inputs.jsonl', flip)
        code, r = self.run_cli()
        self.assertEqual(code, 1)
        d = r['inputs']['comparison']['first_divergence']
        self.assertEqual((d['a']['record']['pressed'], d['b']['record']['pressed']), (0x40, 0))
        self.assertEqual(r['inputs']['b']['pressed_inconsistent_with_held']['examples'], [10])

    def test_held_differs_from_requests(self):
        def change(run):
            def fn(items):
                for f in (10, 11):
                    i = find(items, kind='tick_attempt', frame=f)
                    rec = json.loads(items[i])
                    rec['held'], rec['pressed'] = 0x20, (0x20 if f == 10 else 0)
                    items[i] = json.dumps(rec, separators=(',', ':'))
            self.edit(run, 'inputs.jsonl', fn)
        change(self.a)
        change(self.b)
        code, r = self.run_cli()
        self.assertEqual(code, 1)
        self.assertTrue(r['inputs']['comparison']['identical'])
        self.assertEqual(r['inputs']['a']['held_differs_from_requests']['count'], 2)

    def test_attempt_without_completion(self):
        def drop(items):
            del items[find(items, kind='tick_complete', frame=11)]
        self.edit(self.b, 'inputs.jsonl', drop)
        code, r = self.run_cli()
        self.assertEqual(code, 1)
        self.assertEqual(r['inputs']['b']['unresolved_attempts']['examples'], [11])

    def test_attempt_without_completion_on_both_sides_still_fails(self):
        for run in (self.a, self.b):
            self.edit(run, 'inputs.jsonl', lambda items: items.__delitem__(find(items, kind='tick_complete', frame=11)))
        code, r = self.run_cli()
        self.assertEqual(code, 1)
        self.assertTrue(r['inputs']['comparison']['identical'])
        self.assertFalse(r['inputs']['a']['fully_accounted'])

    def test_failed_final_tick_is_reported_not_hidden(self):
        for run in (self.a, self.b):
            def fail(items):
                i = find(items, kind='tick_complete', frame=15)
                items[i] = json.dumps({'kind': 'error', 'frame': 15, 'message': 'x', 'attempted_tick_frame': 15,
                                       'tick_status': 'failed'})
            self.edit(run, 'inputs.jsonl', fail)
        code, r = self.run_cli()
        self.assertEqual(code, 0, r['failures'])
        self.assertEqual(r['inputs']['a']['failed_attempts'], [15])

    def test_missing_middle_frame(self):
        def drop(items):
            i = find(items, kind='tick_attempt', frame=8)
            del items[i:i + 2]
        self.edit(self.b, 'inputs.jsonl', drop)
        code, r = self.run_cli()
        self.assertEqual(code, 1)
        self.assertEqual(r['inputs']['b']['missing_attempt_frames']['examples'], [[8, 8]])
        self.assertEqual(r['inputs']['comparison']['frames_missing_in_b']['examples'], [[8, 8]])

    def test_explicit_subinterval_compares_only_that_interval(self):
        def flip(items):
            i = find(items, kind='tick_attempt', frame=14)
            rec = json.loads(items[i])
            rec['held'] = rec['pressed'] = 0x8000
            items[i] = json.dumps(rec, separators=(',', ':'))
        self.edit(self.b, 'inputs.jsonl', flip)
        code, r = self.run_cli('--interval', '1:11')
        self.assertEqual(code, 0, r['failures'])
        code, r = self.run_cli('--interval', '1:15')
        self.assertEqual(code, 1)

    def test_malformed_and_missing_inputs_exit_2(self):
        (Path(self.b) / 'inputs.jsonl').write_text('{"kind":"header"}\n{"kind":"tick_attempt","frame":1,\n')
        code, r = self.run_cli()
        self.assertEqual(code, 2)
        self.assertEqual(r['verdict'], 'MALFORMED')
        (Path(self.b) / 'inputs.jsonl').unlink()
        self.assertEqual(self.run_cli()[0], 2)

    def test_missing_request_journal_is_malformed(self):
        (Path(self.b) / 'forwarded-requests.jsonl').unlink()
        self.assertEqual(self.run_cli()[0], 2)

    def test_frames_never_shifted(self):
        # B is A with every status row moved one frame later: no alignment is tried.
        def shift(items):
            for i, line in enumerate(items):
                if line.startswith('{"frame":'):
                    rec = json.loads(line)
                    rec['frame'] += 1
                    items[i] = json.dumps(rec, separators=(',', ':'))
        self.edit(self.b, 'stdout.log', shift)
        code, r = self.run_cli()
        self.assertEqual(code, 1)
        self.assertEqual(r['status']['comparison']['first_divergence']['index'], 0)

if __name__ == '__main__':
    unittest.main()
