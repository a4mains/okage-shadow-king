"""Normal play past the Title boundary: Title -> Restore -> cancel -> Title again (real binary, headless).

Needs OKAGE_NATIVE and the retained replay input local/research/title-lifecycle-validation-v1/title-again-input
(buttons + a copied save; never a user's save directory). The second title construction is history
past the modeled boundary: it must proceed normally and only mark the owner unmodeled, while the
first-title report (written at the first 12c350(1) return) still matches the original.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INPUT = ROOT / "local/research/title-lifecycle-validation-v1/title-again-input"
STAGE = ROOT / "local/game-data-stage-v13"
ORIGINAL = ROOT / "local/research/title-original-v1/analysis"
NATIVE = os.environ.get("OKAGE_NATIVE")

if os.environ.get("OKAGE_REQUIRE_STARTUP_TESTS") and not (NATIVE and INPUT.is_dir() and STAGE.is_dir()):
    raise RuntimeError("required Title title-again test cannot run: binary, replay input or staged root missing")

@unittest.skipUnless(NATIVE and INPUT.is_dir() and STAGE.is_dir(), "OKAGE_NATIVE, replay input or staged root absent")
class TitleAgainTests(unittest.TestCase):
    def test_second_title_construction_proceeds(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            saves = tmp / "saves"
            saves.mkdir()
            shutil.copy(INPUT / "bkmo0.dat", saves / "bkmo0.dat")
            run = subprocess.run([NATIVE, "--game-data", str(STAGE), "--save-directory", str(saves),
                                  "--buttons", str(INPUT / "buttons.txt"), "--title-resource-report", str(tmp / "report"),
                                  "--frames", "1700", "--capture", str(tmp / "f.png")],
                                 capture_output=True, text=True, timeout=900)
            out = run.stdout + run.stderr
            self.assertEqual(run.returncode, 0, out[-3000:])
            self.assertEqual(out.count("Native title constructed"), 2, out[-3000:])
            self.assertIn("Native Restore", out)
            if ORIGINAL.is_dir():
                sys.path.insert(0, str(ROOT / "tools"))
                import title_compare as cmp
                import io
                from contextlib import redirect_stdout
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(cmp.main([str(ORIGINAL), str(tmp / "report")]), 0)

if __name__ == "__main__":
    unittest.main()
