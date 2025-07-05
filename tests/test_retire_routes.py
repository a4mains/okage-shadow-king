"""Retire real-binary routes (headless): Start -> Restore and Start -> Left -> New Game, through title
retirement to entry of 124548, and onward as normal (unmodeled) play.

Needs OKAGE_NATIVE and the staged root. Each route writes --title-retirement-report and an input log;
every consumed input must be a stage file matching the pinned manifest. With the ignored original
evidence present: the *scripted* routes (--title-pad-script, the original observer's presses relative
to title updates) must compare IDENTICAL under --strict (per-update state and timing too); the
fixed-frame routes (different press timing) must compare IDENTICAL in resource history only.
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
ORIGINAL = ROOT / "local/research/retire-title-original-v1/analysis"
ROUTES = ROOT / "tests/data/retire-routes"
MANIFEST_SHA256 = "1696227bf62b137522e12ea2f7529a0138f4ffeea449987901a140b6b57d7ad6"
NATIVE = os.environ.get("OKAGE_NATIVE")
sys.path.insert(0, str(ROOT / "tools"))

if os.environ.get("OKAGE_REQUIRE_STARTUP_TESTS") and not (NATIVE and STAGE.is_dir()):
    raise RuntimeError("required Retire route test cannot run: binary or staged root missing")

@unittest.skipUnless(NATIVE and STAGE.is_dir(), "OKAGE_NATIVE or staged root absent")
class TitleRoutes(unittest.TestCase):
    def run_route(self, route, successor_line, mode, scripted=False):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "saves").mkdir()
            cmd = [NATIVE, "--game-data", str(STAGE), "--save-directory", str(tmp / "saves")]
            if scripted:
                cmd += ["--title-pad-script", str(ROUTES / f"{route}-scripted" / "title-pad.txt")]
            else:
                cmd += ["--buttons", str(ROUTES / route / "buttons.txt")]
                if (ROUTES / route / "directions.txt").exists():
                    cmd += ["--directions", str(ROUTES / route / "directions.txt")]
            cmd += ["--input-log", str(tmp / "reads.jsonl"), "--title-retirement-report", str(tmp / "report"),
                    "--frames", "1200", "--capture", str(tmp / "f.png")]
            run = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
            out = run.stdout + run.stderr
            self.assertEqual(run.returncode, 0, out[-3000:])
            self.assertIn(successor_line, out)  # normal play continues past the stop
            route_json = json.loads((tmp / "report/route.json").read_text())
            self.assertEqual(route_json["route"], route)
            frames = list(route_json["checkpoints"].values())
            self.assertEqual(list(route_json["checkpoints"]), ["first-update", "stable-menu", "removal-request",
                                                               "resource-retirement", "successor-entry"])
            self.assertEqual(frames, sorted(set(frames)))
            # N (removal), N+1 (resource and mode-8 destructors), N+3 (successor entry).
            self.assertEqual(frames[3] - frames[2], 1)
            self.assertEqual(frames[4] - frames[2], 3)
            tails = json.loads((tmp / "report/native-tails.json").read_text())
            self.assertEqual(tails["retiring_effect_slots"], 1)  # water job: reclaim pending at the stop
            self.assertTrue(tails["theme_stop_pending"])
            import retire_input_provenance as prov
            with redirect_stdout(io.StringIO()):
                self.assertEqual(prov.main(["check-reads", str(STAGE), str(tmp / "reads.jsonl"),
                                            "--manifest-sha256", MANIFEST_SHA256, "--strict-stage"]), 0)
            if ORIGINAL.is_dir():
                import retire_title_compare as cmp
                buf = io.StringIO()
                with redirect_stdout(buf):
                    code = cmp.main([str(ORIGINAL), str(tmp / "report")] + (["--strict"] if scripted else []))
                self.assertEqual(code, 0, buf.getvalue()[-4000:])

    def test_restore(self):
        self.run_route("restore", "Native Restore constructed", 10)

    def test_newgame(self):
        self.run_route("newgame", "Native name entry constructed", 9)

    def test_restore_scripted_strict(self):
        self.run_route("restore", "Native Restore constructed", 10, scripted=True)

    def test_newgame_scripted_strict(self):
        self.run_route("newgame", "Native name entry constructed", 9, scripted=True)

if __name__ == "__main__":
    unittest.main()
