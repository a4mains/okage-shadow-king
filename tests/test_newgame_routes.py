"""Newgame real-binary New Game routes (headless, isolated empty save directory).

* scripted: the original observer's inputs (title presses at title updates 2/84/86; name presses consumed at name
  traversals 2..14: erase, Right, append, Start, End, Up, Yes). Checkpoint frames follow the source schedule (death D,
  unlink D+1, publication D+2, retirement D+3, frees D+4, Chapter 1 D+5; counter 240 at chapter call 240, the fade
  completes 50 traversals later, retirement +1, frees +2, the second 0x133830 entry +3). Consumed inputs match the
  pinned stage manifest. The image counter is constant through name entry and steps once per chapter call. With the
  pinned original evidence present the report must compare IDENTICAL (tools/newgame_compare.py --strict).
  Play continues past the stop with explicitly unmodeled ownership.
* no-then-yes: End opens Yes/No on No; Cross on No returns to editing; then End, Up and Yes (focused coverage). The
  resource history at the stop is the original's.
* fixed: the raw-frame tape (tests/data/newgame-routes/newgame-fixed) completes the route; against the original its
  anchors differ, which is ANCHORS-DIFFER (inconclusive), never IDENTICAL.
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
ROUTES = ROOT / "tests/data/newgame-routes"
TITLE_PAD = ROOT / "tests/data/retire-routes/newgame-scripted/title-pad.txt"
ORIGINAL = ROOT / "local/research/newgame-original-v1/analysis"
ORIGINAL_NO = ROOT / "local/research/newgame-no-original-v1"
MANIFEST_SHA256 = "1696227bf62b137522e12ea2f7529a0138f4ffeea449987901a140b6b57d7ad6"
NATIVE = os.environ.get("OKAGE_NATIVE")
sys.path.insert(0, str(ROOT / "tools"))

if os.environ.get("OKAGE_REQUIRE_STARTUP_TESTS") and not (NATIVE and STAGE.is_dir()):
    raise RuntimeError("required Newgame route test cannot run: binary or staged root missing")
# With the requirement set, the original-parity checks must not be silently skipped either.
if os.environ.get("OKAGE_REQUIRE_STARTUP_TESTS") and not (ORIGINAL.is_dir() and ORIGINAL_NO.is_dir()):
    raise RuntimeError("required Newgame route test cannot compare: pinned original analyses missing")

def jsonl(path):
    return [json.loads(x) for x in Path(path).read_text().splitlines()]

@unittest.skipUnless(NATIVE and STAGE.is_dir(), "OKAGE_NATIVE or staged root absent")
class NewGameRoutes(unittest.TestCase):
    def run_route(self, tmp, pad_args, frames=1300):
        saves = tmp / "saves"
        saves.mkdir()
        cmd = [NATIVE, "--game-data", str(STAGE), "--save-directory", str(saves), *pad_args,
               "--input-log", str(tmp / "reads.jsonl"), "--newgame-report", str(tmp / "report"),
               "--frames", str(frames), "--capture", str(tmp / "f.png")]
        run = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        out = run.stdout + run.stderr
        self.assertEqual(run.returncode, 0, out[-3000:])
        self.assertEqual(list(saves.iterdir()), [], "the empty save directory must stay empty")
        route = json.loads((tmp / "report/route.json").read_text())
        c = route["checkpoints"]
        d = c["name-death"]
        self.assertEqual([c[k] - d for k in ("manager-free", "command-published", "resource-retirement", "wrapper-free",
                                              "chapter-constructor-return")], [1, 2, 3, 4, 5])
        ch = c["chapter-constructor-return"]
        self.assertEqual(c["chapter-fade-request"] - ch, 239)  # the 240th 0x133a98 call (the first runs at construction)
        self.assertEqual(c["chapter-mode-request"] - c["chapter-fade-request"], 50)  # actual fade completion
        self.assertEqual([c[k] - c["chapter-mode-request"] for k in ("chapter-resource-retirement",
                                                                       "chapter-wrapper-free", "field-constructor-entry")],
                         [1, 2, 3])
        session = {k: json.loads((tmp / "report" / k / "session.json").read_text()) for k in c}
        counter = {k: v["image_counter"] for k, v in session.items()}
        self.assertEqual(len({counter[k] for k in ("name-constructor-return", "name-death", "command-published",
                                                   "chapter-constructor-return")}), 1,
                         f"name entry must not step the image counter (no image list): {counter}")
        self.assertEqual(counter["field-constructor-entry"] - counter["chapter-constructor-return"], 290,
                         "Chapter 1 steps the counter once per 0x133a98 call")
        self.assertEqual(session["command-published"]["command"], [14, 1, 0])
        self.assertEqual(session["field-constructor-entry"]["command"], [0, 0, 0])
        self.assertEqual(session["field-constructor-entry"]["mode_parent"]["current"], [2, 72, 0])
        self.assertEqual(session["command-published"]["name_844"], session["field-constructor-entry"]["name_844"],
                         "Chapter 1 clears temporary variables but preserves the published name")
        self.assertIn("Native ordinary field queued scene=72 entrance=0", out)
        import retire_input_provenance as prov
        with redirect_stdout(io.StringIO()):
            self.assertEqual(prov.main(["check-reads", str(STAGE), str(tmp / "reads.jsonl"),
                                        "--manifest-sha256", MANIFEST_SHA256, "--strict-stage"]), 0)
        return out, route

    def compare_original(self, tmp, want):
        if not ORIGINAL.is_dir():
            return None
        import newgame_compare as cmp
        buf = io.StringIO()
        with redirect_stdout(buf):
            code = cmp.main([str(ORIGINAL), str(tmp / "report"), "--strict", "--json", str(tmp / "compare.json")])
        self.assertEqual(code, want, buf.getvalue()[-4000:])
        return json.loads((tmp / "compare.json").read_text())

    def test_scripted_original_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            out, route = self.run_route(tmp, ["--title-pad-script", str(TITLE_PAD), "--name-pad-script",
                                              str(ROUTES / "newgame-chapter/name-pad.txt")], frames=1400)
            frames = jsonl(tmp / "report/name-frames.jsonl")
            self.assertEqual([[r["t"], r["pressed"]] for r in frames if r["pressed"]],
                             [[2, 0x20], [4, 0x2000], [6, 0x40], [8, 0x800], [10, 0x40], [12, 0x1000], [14, 0x40]])
            self.assertEqual(route["checkpoints"]["field-constructor-entry"] - route["construction_frame"], 344)
            result = self.compare_original(tmp, 0)
            if result is not None:
                self.assertEqual(result["result"], "IDENTICAL")
                self.assertEqual(result["asserted"], {"lifecycle": 0, "rng": 0, "frames": 0})

    def test_no_then_yes(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            self.run_route(tmp, ["--title-pad-script", str(TITLE_PAD), "--name-pad-script",
                                 str(ROUTES / "no-then-yes/name-pad.txt")])
            frames = jsonl(tmp / "report/name-frames.jsonl")
            back = [r for r in frames if r["manager_ran"] and r["phase_in"] == -1 and r["phase_out"] == 1]
            self.assertEqual(len(back), 1, "exactly one No returns from Yes/No to editing")
            self.assertEqual((back[0]["pressed"], back[0]["yes_no_out"]), (0x40, 1))
            accept = [r for r in frames if r["manager_ran"] and r["phase_in"] == -1 and r["phase_out"] == -2]
            self.assertEqual(len(accept), 1)
            if ORIGINAL_NO.is_dir():
                import newgame_compare as cmp
                buf = io.StringIO()
                with redirect_stdout(buf):
                    code = cmp.main([str(ORIGINAL_NO / "analysis"), str(tmp / "report"), "--strict", "--accept-control-timing",
                                     "--manifest", str(ROOT / "docs/newgame-no-original-manifest.json")])
                self.assertEqual(code, 0, buf.getvalue()[-4000:])
            if ORIGINAL.is_dir():
                # The No branch changes no resource history: the stop's lifecycle rows equal the original's.
                import newgame_compare as cmp
                o, _ = cmp.load_lifecycle(ORIGINAL / "field-constructor-entry/events.jsonl")
                n, a = cmp.load_lifecycle(tmp / "report/field-constructor-entry/lifecycle.jsonl", native=True)
                self.assertEqual([cmp.canon(e) for e in o if e["kind"] != "phase"],
                                 [cmp.canon(e) for e in n if e["kind"] != "phase"])
                self.assertFalse(any(a))

    def test_no_then_yes_control_timeline(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            self.run_route(tmp, ["--title-pad-script", str(TITLE_PAD), "--name-pad-script",
                                 str(ROUTES / "no-then-yes-control/name-pad.txt")])
            if ORIGINAL_NO.is_dir():
                import newgame_control_exactness as ce
                code, r = ce.run(ORIGINAL_NO / "runs/control-minimal-fill00", tmp / "report")
                self.assertEqual(code, 0, json.dumps(r)[:3000])

    def test_fixed_raw_frame_tape(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            fixed = ROUTES / "newgame-fixed"
            self.run_route(tmp, ["--buttons", str(fixed / "buttons.txt"), "--directions", str(fixed / "directions.txt")],
                           frames=1600)
            result = self.compare_original(tmp, 3)
            if result is not None:
                self.assertTrue(result["anchors_differ"])

if __name__ == "__main__":
    unittest.main()
