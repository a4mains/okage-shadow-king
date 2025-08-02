"""Restore real-binary Restore routes (headless, isolated empty save directory).

* restore-cancel: title -> Restore -> empty-slot ready UI -> Cancel -> second title constructor return,
  then normal (unmodeled) play. Checkpoint frames follow the source schedule (death C+32, request D+2,
  retirement D+3, frees D+4, second title D+5); consumed inputs match the pinned stage manifest; with the
  pinned original evidence present the report must compare IDENTICAL (tools/restore_compare.py).
* pending-cancel: Cancel while the storage check job is still running (research latency option). The
  window dies at C+32, the pending job is joined, the second title is constructed (lifetime regression).
"""
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / "local/game-data-stage-v13"
ROUTES = ROOT / "tests/data/restore-routes"
ORIGINAL = ROOT / "local/research/restore-original-v1/analysis"
MANIFEST_SHA256 = "1696227bf62b137522e12ea2f7529a0138f4ffeea449987901a140b6b57d7ad6"
NATIVE = os.environ.get("OKAGE_NATIVE")
sys.path.insert(0, str(ROOT / "tools"))

if os.environ.get("OKAGE_REQUIRE_STARTUP_TESTS") and not (NATIVE and STAGE.is_dir()):
    raise RuntimeError("required Restore route test cannot run: binary or staged root missing")

@unittest.skipUnless(NATIVE and STAGE.is_dir(), "OKAGE_NATIVE or staged root absent")
class RestoreRoutes(unittest.TestCase):
    def run_route(self, tmp, route, extra=()):
        saves = tmp / "saves"
        saves.mkdir()
        pad = (["--buttons", str(ROUTES / route / "buttons.txt")] if (ROUTES / route / "buttons.txt").exists() else [])
        cmd = [NATIVE, "--game-data", str(STAGE), "--save-directory", str(saves), *pad, "--input-log", str(tmp / "reads.jsonl"),
               "--restore-cancel-report", str(tmp / "report"), *extra, "--frames", "1300", "--capture", str(tmp / "f.png")]
        run = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        out = run.stdout + run.stderr
        self.assertEqual(run.returncode, 0, out[-3000:])
        self.assertEqual(list(saves.iterdir()), [], "the empty save directory must stay empty")
        self.assertEqual(out.count("Native title constructed"), 2, out[-3000:])
        route_json = json.loads((tmp / "report/route.json").read_text())
        c = route_json["checkpoints"]
        self.assertEqual(c["resource-retirement"] - c["cancel-death"], 3)
        self.assertEqual(c["wrapper-free"] - c["cancel-death"], 4)
        self.assertEqual(c["second-title-return"] - c["cancel-death"], 5)
        import retire_input_provenance as prov
        with redirect_stdout(io.StringIO()):
            self.assertEqual(prov.main(["check-reads", str(STAGE), str(tmp / "reads.jsonl"),
                                        "--manifest-sha256", MANIFEST_SHA256, "--strict-stage"]), 0)
        return out, route_json

    def test_restore_cancel(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            out, route = self.run_route(tmp, "restore-cancel")
            self.assertEqual(route["checkpoints"]["cancel-death"] - 1210, 32)  # Cancel consumed at 1210
            self.compare_original(tmp)

    def compare_original(self, tmp):
        """Every checkpoint's lifecycle/state/effect/VRAM must equal the original. Native card readiness
        is earlier than the original's (host IO), so per-traversal/RNG parity is expected to be UNRESOLVED
        (TIMING-DIFFERENT, exit 3) with the timing diagnostic holding; anything else fails."""
        if not ORIGINAL.is_dir():
            return
        import restore_compare as cmp
        buf = io.StringIO()
        with redirect_stdout(buf):
            code = cmp.main([str(ORIGINAL), str(tmp / "report"), "--json", str(tmp / "compare.json")])
        self.assertIn(code, (0, 3), buf.getvalue()[-4000:])
        result = json.loads((tmp / "compare.json").read_text())
        for cp, c in result["checkpoints"].items():
            self.assertTrue(c["events_identical"] and not c["state_differences"] and c["effect106_identical"]
                            and not c["stream_inconsistent"], cp)
        if code == 3:
            self.assertTrue(result["timing_diagnostic"]["lcg_same"] and result["timing_diagnostic"]["pass_model_same"])

    def test_restore_cancel_original_pad_rule(self):
        # The original observer's inputs: title presses at title updates 2/84, Cancel on the traversal
        # after the first input-accepting 0x21c traversal.
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            out, route = self.run_route(tmp, "scripted", [
                "--title-pad-script", str(ROOT / "tests/data/retire-routes/restore-scripted/title-pad.txt"),
                "--restore-cancel-after-ready", "1"])
            c = route["checkpoints"]
            self.assertEqual(c["cancel-death"] - c["empty-ready"], 33)  # Cancel read at ready+1, death 32 later
            self.compare_original(tmp)

    def test_pending_scan_cancel(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            out, route = self.run_route(tmp, "pending-cancel", ["--restore-storage-latency-ms", "3000"])
            self.assertEqual(route["checkpoints"]["cancel-death"] - 1070, 32)

if __name__ == "__main__":
    unittest.main()
