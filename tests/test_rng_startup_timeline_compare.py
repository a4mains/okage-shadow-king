"""Controls for tools/rng_startup_timeline_compare.py (needs ignored Startup/RNG evidence)."""
import io
import json
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import rng_startup_timeline_compare as tl  # noqa: E402

EVENTS = ROOT / "local/research/startup-original-v1/runs/fill00-a/events.jsonl"
NATIVE = ROOT / "local/research/rng-session-native-v1/report"

@unittest.skipUnless(EVENTS.is_file() and (NATIVE / "timeline.jsonl").is_file(), "RNG timeline evidence absent")
class TimelineCompareTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.nat = Path(self.tmp.name) / "native"
        self.nat.mkdir()
        for name in ("timeline.jsonl", "state.json"):
            shutil.copy(NATIVE / name, self.nat / name)
        self.lines = (NATIVE / "timeline.jsonl").read_text().splitlines()

    def run_cmp(self, events=EVENTS):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = tl.main([str(events), str(self.nat)])
        return code, out.getvalue()

    def put(self, lines):
        (self.nat / "timeline.jsonl").write_text("\n".join(lines) + "\n")

    def edit(self, index, **changes):
        lines = list(self.lines)
        d = json.loads(lines[index])
        d.update(changes)
        lines[index] = json.dumps(d)
        return lines

    def alloc_index(self, alloc_id):
        return next(i for i, l in enumerate(self.lines)
                    if json.loads(l)["kind"] == "alloc" and json.loads(l)["alloc"] == alloc_id)

    def test_real_timeline_matches_original(self):
        code, out = self.run_cmp()
        self.assertEqual(code, 0, out)
        self.assertIn("result: IDENTICAL", out)

    def test_shifted_draw_position_is_different(self):
        # Effect preparation moved before the last effect upload: same totals, wrong order.
        i = self.alloc_index(72)
        self.put(self.edit(i, draws=9504))
        self.assertEqual(self.run_cmp()[0], 1)
        i = self.alloc_index(6)
        self.put(self.edit(i, draws=512))
        self.assertEqual(self.run_cmp()[0], 1)

    def test_swapped_allocations_are_different(self):
        a, b = self.alloc_index(9), self.alloc_index(10)
        lines = list(self.lines)
        lines[a], lines[b] = lines[b], lines[a]
        self.put(lines)
        self.assertEqual(self.run_cmp()[0], 1)

    def test_missing_event_or_draw_total_is_malformed(self):
        lines = list(self.lines)
        del lines[self.alloc_index(40)]
        self.put(lines)
        self.assertEqual(self.run_cmp()[0], 2)
        state = json.loads((NATIVE / "state.json").read_text())
        state["rng_draws"] = 9503
        (self.nat / "state.json").write_text(json.dumps(state))
        self.put(self.lines)
        self.assertEqual(self.run_cmp()[0], 2)

    def test_unpinned_original_and_bad_entries_are_malformed(self):
        copy = Path(self.tmp.name) / "events.jsonl"
        copy.write_text(EVENTS.read_text().replace('"rand"', '"rand" ', 1))
        self.assertEqual(self.run_cmp(copy)[0], 2)
        self.put(self.edit(3, extra=1))
        self.assertEqual(self.run_cmp()[0], 2)
        self.put([self.lines[0].replace('"kind"', '"kind": "alloc", "kind"', 1)] + self.lines[1:])
        self.assertEqual(self.run_cmp()[0], 2)
        self.assertEqual(tl.main([]), 2)

if __name__ == "__main__":
    unittest.main()
