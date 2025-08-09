"""tools/restore_io_timing_evidence.py on the retained evidence (skips when the ignored evidence is absent)."""
import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import restore_io_timing_evidence as ev  # noqa: E402

NATIVE = ROOT / "local/research/restore-ownership-validation-v1/final/sessions/release-scripted/report"

@unittest.skipUnless(NATIVE.is_dir() and (ev.ORIG / "runs/control-minimal-fill00").is_dir(), "Restore evidence absent")
class IoTimingEvidence(unittest.TestCase):
    def test_one_model_predicts_every_run_and_originals_disagree(self):
        with redirect_stdout(io.StringIO()):
            code, r = ev.run(str(NATIVE))
        self.assertEqual(code, 0, r)
        self.assertFalse(r["original_runs_agree_at_fixed_traversals"])
        self.assertNotEqual(r["original-hooked"]["draws_to_second_title"], r["original-control"]["draws_to_second_title"])

    def test_model_rejects_a_wrong_readiness(self):
        self.assertNotEqual(ev.predicted(2, 6, ev.DEATH_T), ev.predicted(2, 7, ev.DEATH_T))

class ModelUnits(unittest.TestCase):
    def test_lcg_steps(self):
        s = 12345
        t = s
        for _ in range(10):
            t = (t * 0x41C64E6D + 0x3039) & 0xFFFFFFFF
        self.assertEqual(ev.lcg_steps(s, t), 10)

if __name__ == "__main__":
    unittest.main()
