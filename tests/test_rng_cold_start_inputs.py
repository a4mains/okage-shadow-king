"""Real headless cold Session against required imported startup inputs (RNG).

Needs OKAGE_NATIVE (okage-native executable) and the staged root local/game-data-stage-v13.
Each case builds a fresh symlink overlay of the staged root with one change and runs the
actual cold startup for one frame headlessly (no window).
"""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / "local/game-data-stage-v13"
NATIVE = os.environ.get("OKAGE_NATIVE")

def overlay(src, dst, replace):
    """Mirror src as directories + symlinks; `replace` maps relative path -> bytes or None (omit)."""
    for root, dirs, files in os.walk(src):
        rel = Path(root).relative_to(src)
        (dst / rel).mkdir(parents=True, exist_ok=True)
        for name in files + [d for d in dirs if (Path(root) / d).is_symlink()]:
            r = str(rel / name) if str(rel) != "." else name
            if r in replace:
                if replace[r] is not None:
                    (dst / r).write_bytes(replace[r])
                continue
            os.symlink(os.path.realpath(Path(root) / name), dst / r)

@unittest.skipUnless(NATIVE and STAGE.is_dir(), "OKAGE_NATIVE or staged root absent")
class ColdStartInputTests(unittest.TestCase):
    def run_cold(self, replace, report=False):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            root = tmp / "root"
            overlay(STAGE, root, replace)
            self.assertFalse((root / "SCUS_971.29").exists())
            cmd = [NATIVE, "--game-data", str(root), "--save-directory", str(tmp / "saves"),
                   "--frames", "1", "--capture", str(tmp / "f.png")]
            if report:
                cmd += ["--startup-texture-report", str(tmp / "report")]
            run = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            produced = (tmp / "report" / "state.json").is_file()
            return run.returncode, run.stdout + run.stderr, produced

    def corrupt(self, rel):
        data = bytearray((STAGE / rel).resolve().read_bytes())
        data[len(data) // 2] ^= 0x01
        return bytes(data)

    def test_valid_import_without_elf_builds_owner(self):
        code, out, produced = self.run_cold({}, report=True)
        self.assertEqual(code, 0, out)
        self.assertIn("rng_draws=9504", out)
        self.assertTrue(produced)

    def test_missing_or_corrupt_required_inputs_fail_clearly(self):
        cases = {
            "missing manifest": {"NATIVE_STARTUP/startup-inputs.json": None},
            "missing palette": {"NATIVE_STARTUP/FONTCLUT.BIN": None},
            "corrupt palette": {"NATIVE_STARTUP/FONTCLUT.BIN": self.corrupt("NATIVE_STARTUP/FONTCLUT.BIN")},
            "corrupt font": {"CMNDATA/FONT/BM.FNT": self.corrupt("CMNDATA/FONT/BM.FNT")},
            "corrupt effect": {"CMNDATA/2D/ENV_TEX.XPF/niji.xim": self.corrupt("CMNDATA/2D/ENV_TEX.XPF/niji.xim")},
            "corrupt efd": {"CMNDATA/2D/TES.EFD": self.corrupt("CMNDATA/2D/TES.EFD")},
        }
        for name, replace in cases.items():
            with self.subTest(name):
                code, out, _ = self.run_cold(replace)
                self.assertNotEqual(code, 0, out)
                self.assertNotIn("Native startup textures:", out)
                self.assertIn("import_startup_inputs.py", out)

if __name__ == "__main__":
    unittest.main()
