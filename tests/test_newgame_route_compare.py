"""Controls for tools/newgame_route_compare.py using synthetic run directories.

The fixture writes runs consistent with the route's own stdout events: interval streams carry records exactly while
their owner is live, and game-mode.bin carries (frame, point) records following the documented C/R/Q rule (written
here independently of the tool). A clean pair is IDENTICAL; every defect is injected explicitly.
"""
import io
import json
import shutil
import sys
import tempfile
import unittest
from array import array
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import newgame_route_compare as rc  # noqa: E402

N, PER, RECLEN, GM = 40, 4, 5, 25

def events(q=38, c=32, retired=36):
    return [("Native startup constructed selector=3", 2), ("Native startup constructed selector=0", 5),
            ("Native startup requested selector=1", 9), ("Native title constructed", 11),
            ("Native title resource/task destroyed", 20), ("Native name entry constructed", 22),
            ("Native name entry requests opening field", 28), ("Native name entry retired", 30),
            ("Native chapter constructed", c), ("Native chapter requests ordinary field", 34),
            ("Native chapter retired", retired), ("Native ordinary field queued", q)]

def game_mode_keys(c, r, q):
    keys = set()
    for f in range(1, N + 1):
        if f < c or r < f < q:
            keys |= {(f, 0), (f, 1)}
        elif f in (c, q):
            keys.add((f, 0))
        elif f == r:
            keys.add((f, 1))
    return keys

def make_run(d, q=38, c=32, retired=36, omit=None, extra_gm=(), omit_gm=(), tweak=None, gm_word=None,
             exit_text="0", command=None):
    """omit: {stream: set of frames}; tweak: {stream: {frame: delta on last word}}; gm_word: {(frame, point): delta}."""
    d.mkdir()
    ev = events(q, c, retired)
    (d / "command.txt").write_text(command or f"okage-native --frames {N} --capture {d}/f.png\n")
    (d / "stdout.txt").write_text("".join(f"{t} frame={f}\n" for t, f in ev))
    (d / "exit.txt").write_text(exit_text + "\n")
    (d / "f.png").write_bytes(b"png")
    omit, tweak = omit or {}, tweak or {}
    intervals = {"startup": (set(range(2, 10)), 12), "title-state": (set(range(11, 20)), RECLEN),
                 "name-entry-state": (set(range(22, 30)), RECLEN),
                 "chapter-state": (set(range(c, retired)), 6)}
    for s, (frames, length) in intervals.items():
        w = array("I")
        for f in sorted(frames - set(omit.get(s, ()))):
            rec = [f] + [1000 + f * 3 + k for k in range(length - 1)]
            rec[-1] += tweak.get(s, {}).get(f, 0)
            w.extend(rec)
        (d / f"f.png.{s}.bin").write_bytes(w.tobytes())
    w = array("I")
    keys = (game_mode_keys(c, 34, q) - set(omit_gm)) | set(extra_gm)
    for key in sorted(keys):
        rec = [key[0], key[1]] + [2000 + key[0] * 7 + k for k in range(GM - 2)]
        rec[-1] += (gm_word or {}).get(key, 0)
        w.extend(rec)
    (d / "f.png.game-mode.bin").write_bytes(w.tobytes())
    a = array("h", [i % 50 for i in range(N * PER)])
    (d / "f.png.audio.pcm").write_bytes(a.tobytes())

class RouteCompare(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.json = self.base / "o.json"

    def run_pair(self, a_kw=None, b_kw=None):
        a, b = self.base / "a", self.base / "b"
        make_run(a, **(a_kw or {}))
        make_run(b, **(b_kw or {}))
        self.out = io.StringIO()
        with redirect_stdout(self.out), redirect_stderr(self.out):
            code = rc.main([str(a), str(b), "--json", str(self.json)])
        return code

    def report(self):
        return json.loads(self.json.read_text())

    def test_identical(self):
        self.assertEqual(self.run_pair(), 0, self.out.getvalue())
        self.assertIn("result: IDENTICAL", self.out.getvalue())
        r = self.report()
        self.assertEqual(r["incomplete"], [])
        self.assertEqual(r["streams"]["chapter-state.bin"]["coverage"]["a"]["expected"], "32-35")
        self.assertEqual(r["streams"]["game-mode.bin"]["coverage"]["b"]["expected"],
                         len(game_mode_keys(32, 34, 38)))

    def test_chapter_state_record_missing_one_side(self):
        self.assertEqual(self.run_pair({}, {"omit": {"chapter-state": {33}}}), 4, self.out.getvalue())
        out = self.out.getvalue()
        self.assertIn("result: INCOMPLETE", out)
        self.assertIn("INCOMPLETE run b: missing 33", out)
        c = self.report()["streams"]["chapter-state.bin"]["coverage"]
        self.assertEqual((c["a"]["missing"], c["b"]["missing"]), ("none", "33"))

    def test_chapter_state_record_missing_both_sides(self):
        kw = {"omit": {"chapter-state": {33}}}
        self.assertEqual(self.run_pair(kw, kw), 4, self.out.getvalue())
        self.assertIn("result: INCOMPLETE", self.out.getvalue())
        r = self.report()
        self.assertIsNone(r["streams"]["chapter-state.bin"]["first_difference_frame"])  # the runs agree
        self.assertEqual(r["streams"]["chapter-state.bin"]["coverage"]["a"]["missing"], "33")

    def test_game_mode_record_at_steady_mode2_frame(self):
        # frame 33 lies between C and R: the parent is in steady mode 2 and writes no record
        self.assertEqual(self.run_pair({}, {"extra_gm": {(33, 0)}}), 4, self.out.getvalue())
        self.assertIn("unexpected 33.0", self.out.getvalue())
        self.assertIn("result: INCOMPLETE", self.out.getvalue())

    def test_game_mode_record_missing_both_sides(self):
        kw = {"omit_gm": {(20, 1)}}
        self.assertEqual(self.run_pair(kw, kw), 4, self.out.getvalue())
        self.assertIn("missing 20.1", self.out.getvalue())

    def test_chapter_state_word_differs(self):
        self.assertEqual(self.run_pair({}, {"tweak": {"chapter-state": {34: 1}}}), 1, self.out.getvalue())
        r = self.report()
        self.assertTrue(r["anchors_equal"])
        self.assertEqual(r["verdict"], "DIFFERENT")
        s = r["streams"]["chapter-state.bin"]
        self.assertEqual((s["first_difference_frame"], s["last_difference_frame"]), (34, 34))
        self.assertIn("result: DIFFERENT", self.out.getvalue())

    def test_game_mode_word_differs(self):
        self.assertEqual(self.run_pair({}, {"gm_word": {(35, 1): 5}}), 1, self.out.getvalue())
        g = self.report()["streams"]["game-mode.bin"]
        self.assertEqual(g["first_difference"], [35, 1])
        self.assertEqual(g["differing_words"], [GM - 1])

    def test_anchors_differ(self):
        # b queues the ordinary field one frame later; its streams follow its own events
        self.assertEqual(self.run_pair({}, {"q": 39}), 3, self.out.getvalue())
        out = self.out.getvalue()
        self.assertIn("result: ANCHORS-DIFFER", out)
        self.assertIn("not attributed to timing", out)
        r = self.report()
        self.assertFalse(r["anchors_equal"])
        self.assertEqual(r["incomplete"], [])

    def test_anchors_differ_chapter_interval_shift(self):
        self.assertEqual(self.run_pair({}, {"retired": 37}), 3, self.out.getvalue())
        self.assertEqual(self.report()["streams"]["chapter-state.bin"]["coverage"]["b"]["expected"], "32-36")

    def test_identical_command_txt_malformed(self):
        cmd = f"okage-native --frames {N} --capture shared/f.png\n"
        self.assertEqual(self.run_pair({"command": cmd}, {"command": cmd}), 2, self.out.getvalue())
        self.assertIn("identical command.txt", self.out.getvalue())

    def test_missing_route_event_malformed(self):
        a, b = self.base / "a", self.base / "b"
        make_run(a)
        make_run(b)
        text = (b / "stdout.txt").read_text().replace("Native chapter retired", "Native chapter gone")
        (b / "stdout.txt").write_text(text)
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            self.assertEqual(rc.main([str(a), str(b)]), 2)
        self.assertIn("Native chapter retired", out.getvalue())

    def test_final_png_differs(self):
        a, b = self.base / "a", self.base / "b"
        make_run(a)
        make_run(b)
        (b / "f.png").write_bytes(b"other")
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            self.assertEqual(rc.main([str(a), str(b)]), 1)
        self.assertIn("final PNG: different", out.getvalue())

if __name__ == "__main__":
    unittest.main()
