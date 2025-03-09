"""Native vs retained original-execution lifecycle traces (Handle).

Runs okage-texture-handle-trace over every retained op script and requires an
identical trace to the one recorded from the unchanged retail routines under the
Play research backend. Skipped when the ignored evidence or the binary is absent.
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "local/research/handle-lifecycle-v1"
TOOL = Path(os.environ.get("OKAGE_HANDLE_TRACE", ROOT / "build/handle-trace-release/okage-texture-handle-trace"))
DIFF = ROOT / "tools/texture_handle_trace_diff.py"

@unittest.skipUnless(EVIDENCE.is_dir() and TOOL.is_file(), "lifecycle evidence or trace binary missing")
class LifecycleTests(unittest.TestCase):
    def scripts(self):
        return sorted((EVIDENCE / "scripts").glob("*.txt"))

    def test_every_scenario_matches_original(self):
        scripts = self.scripts()
        self.assertGreaterEqual(len(scripts), 19)
        with tempfile.TemporaryDirectory() as tmp:
            for script in scripts:
                with self.subTest(script=script.name):
                    out = Path(tmp) / (script.stem + ".jsonl")
                    run = subprocess.run([str(TOOL), str(script), str(out)], capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stderr)
                    diff = subprocess.run([sys.executable, "-I", str(DIFF),
                                           str(EVIDENCE / "original" / (script.stem + ".jsonl")), str(out)],
                                          capture_output=True, text=True)
                    self.assertEqual(diff.returncode, 0, diff.stdout[-2000:])

if __name__ == "__main__":
    unittest.main()
