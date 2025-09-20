"""tools/newgame_control_exactness.py on the retained No-branch control (skipped when the evidence is absent)."""
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import newgame_control_exactness as ce  # noqa: E402

CONTROL = ROOT / "local/research/newgame-no-original-v1/runs/control-minimal-fill00"
POST = ROOT / "local/research/newgame-postfix-sessions-v1"

@unittest.skipUnless(CONTROL.is_dir() and POST.is_dir(), "retained Newgame No-branch evidence absent")
class ControlExactness(unittest.TestCase):
    def test_control_timeline_exact(self):
        self.assertEqual(ce.run(CONTROL, POST / "release-no-control/report")[0], 0)

    def test_dense_timeline_is_not_the_control(self):
        self.assertEqual(ce.run(CONTROL, POST / "release-no-dense/report")[0], 1)

    def test_truncated_known_mask_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            rep = Path(tmp) / "report"
            shutil.copytree(POST / "release-no-control/report", rep)
            os.chmod(rep / "name-confirmed", 0o755)
            k = rep / "name-confirmed/vram.known"
            os.chmod(k, 0o644)
            k.write_bytes(b"")
            with self.assertRaises(ValueError):
                ce.run(CONTROL, rep)

if __name__ == "__main__":
    unittest.main()
