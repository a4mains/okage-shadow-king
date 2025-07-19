"""Controls for tools/retire_route_compare.py using synthetic run directories.

The fixture generator writes runs consistent with a route's own events: stream records appear exactly
on the frames the events say each owner is live (rules in the comparator docstring), so a clean pair
is IDENTICAL and every defect is injected explicitly.
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
import retire_route_compare as rc  # noqa: E402

N, PER, RECLEN = 40, 4, 5
SUFFIXES = ("startup", "title-state", "restore-state", "name-entry-state", "game-mode")

def route_spec(route, late=0, drop_events=()):
    """Return (ordered [(event text, frame)], {stream: set of frames with records}) for a route.

    `late` shifts every event from the title onward (the startup part stays put)."""
    ev = [("Native startup constructed selector=3", 2), ("Native startup constructed selector=0", 5),
          ("Native startup requested selector=1", 9), ("Native title constructed", 11 + late),
          ("Native title resource/task destroyed", 20 + late)]
    if route == "newgame":
        ev.append(("Native name entry constructed", 22 + late))
    else:
        ev.append(("Native Restore constructed", 22 + late))
    if route == "restore-cancel":
        ev += [("Native Restore retired", 30 + late), ("Native title constructed", 32 + late)]
    ev = [e for e in ev if e[0] not in drop_events]
    iv = {s: set() for s in SUFFIXES}
    iv["startup"] = set(range(2, 10))  # constructed(2) .. requested(9) inclusive
    iv["game-mode"] = set(range(1, N + 1))
    iv["title-state"] = set(range(11 + late, 20 + late))  # constructed .. frame before destroyed
    if route == "restore":
        iv["restore-state"] = set(range(22 + late, N + 1))  # unclosed: runs to the final frame
    if route == "newgame":
        iv["name-entry-state"] = set(range(22 + late, N + 1))
    if route == "restore-cancel":
        iv["restore-state"] = set(range(22 + late, 30 + late))
        iv["title-state"] |= set(range(32 + late, N + 1))
    return ev, iv

def make_run(d, route="restore", late=0, drop_events=(), omit=None, extra=None, tweak=None, word_tweak=None,
             audio_tweak=None, png=b"png", exit_text="0", skip=(), partial=(), dup=None, audio_len=None,
             frames=N, empty=(), no_events=False):
    """omit/extra: {stream: set of frames removed/added}; tweak: {stream: {frame: delta on last word}};
    word_tweak: {stream: {(frame, word): delta}}."""
    d.mkdir()
    ev, iv = route_spec(route, late, drop_events)
    (d / "command.txt").write_text(f"okage-native --frames {frames} --capture {d}/f.png\n")
    (d / "stdout.txt").write_text("nothing here\n" if no_events else "".join(f"{t} frame={f}\n" for t, f in ev))
    if exit_text is not None:
        (d / "exit.txt").write_text(exit_text + "\n")
    (d / "f.png").write_bytes(png)
    omit, extra, tweak, word_tweak = omit or {}, extra or {}, tweak or {}, word_tweak or {}
    for s in SUFFIXES:
        length = 12 if s == "startup" else RECLEN
        w = array("I")
        if s not in empty:
            for f in sorted((iv[s] - set(omit.get(s, ()))) | set(extra.get(s, ()))):
                rec = [f] + [f * 3 + k for k in range(length - 1)]
                rec[-1] += tweak.get(s, {}).get(f, 0)
                for (tf, tw), delta in word_tweak.get(s, {}).items():
                    if tf == f:
                        rec[tw] += delta
                w.extend(rec)
            if dup == s:
                w.extend([2] + [0] * (length - 1))
            if s in partial:
                w.append(7)
        if s not in skip:
            (d / f"f.png.{s}.bin").write_bytes(w.tobytes())
    n = frames * PER if audio_len is None else audio_len
    a = array("h", [i % 50 for i in range(n)])
    if audio_tweak is not None:
        a[audio_tweak] += 1
    (d / "f.png.audio.pcm").write_bytes(a.tobytes())

class RouteCompare(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.json = self.base / "o.json"

    def clean(self):
        for x in ("a", "b"):
            shutil.rmtree(self.base / x, ignore_errors=True)

    def run_pair(self, a_kw=None, b_kw=None, route="restore", extra=None, same=False, route_arg=True):
        a, b = self.base / "a", self.base / "b"
        make_run(a, **{"route": route, **(a_kw or {})})
        if same:
            b = a
        else:
            make_run(b, **{"route": route, **(b_kw or {})})
        args = [str(a), str(b)] + (["--route", route] if route_arg else []) + list(extra or [])
        self.out = io.StringIO()
        with redirect_stdout(self.out), redirect_stderr(self.out):
            return rc.main(args)

    def jrun(self, *a, **k):
        k["extra"] = ["--json", str(self.json)] + list(k.get("extra") or [])
        return self.run_pair(*a, **k)

    def report(self):
        return json.loads(self.json.read_text())

    # ---- clean runs per route
    def test_identical_complete_runs_per_route(self):
        for route in ("restore", "newgame", "restore-cancel"):
            with self.subTest(route=route):
                self.clean()
                self.assertEqual(self.jrun(route=route), 0, self.out.getvalue())
                self.assertIn("result: IDENTICAL", self.out.getvalue())
                r = self.report()
                self.assertEqual(r["verdict"], "IDENTICAL")
                self.assertEqual(r["incomplete"], [])

    def test_restore_cancel_has_two_title_intervals(self):
        self.assertEqual(self.jrun(route="restore-cancel"), 0)
        s = self.report()["streams"]
        self.assertEqual(s["title-state.bin"]["coverage"]["a"]["expected"], "11-19,32-40")
        self.assertEqual(s["title-state.bin"]["frames_a"], "11-19,32-40")
        self.assertEqual(s["restore-state.bin"]["coverage"]["b"]["expected"], "22-29")
        self.assertEqual(s["startup.bin"]["coverage"]["a"]["expected"], "2-9")
        self.assertEqual(s["game-mode.bin"]["coverage"]["a"]["expected"], "1-40")
        self.assertEqual(s["name-entry-state.bin"]["coverage"]["a"]["expected"], "none")

    def test_json_coverage_per_stream(self):
        self.assertEqual(self.jrun(route="newgame"), 0)
        r = self.report()
        self.assertEqual(r["route"], "newgame")
        self.assertEqual(r["frames"], N)
        self.assertEqual(sorted(r["streams"]), sorted(rc.STREAMS))
        for name, s in r["streams"].items():
            for side in "ab":
                c = s["coverage"][side]
                self.assertEqual((c["missing"], c["unexpected"]), ("none", "none"), name)
                self.assertEqual(c["expected"], s["frames_" + side], name)
        self.assertEqual(r["streams"]["name-entry-state.bin"]["coverage"]["a"]["expected"], "22-40")
        self.assertEqual(r["audio"]["samples"], N * PER)

    # ---- payload differences with equal anchors
    def test_payload_change_equal_anchors_exact_frames(self):
        code = self.jrun({"tweak": {"game-mode": {9: 1}}}, {"tweak": {"game-mode": {14: 1}}})
        self.assertEqual(code, 1)
        r = self.report()
        s = r["streams"]["game-mode.bin"]
        self.assertEqual((s["first_difference_frame"], s["last_difference_frame"]), (9, 14))
        self.assertTrue(r["anchors_equal"])
        self.assertEqual(r["verdict"], "DIFFERENT")
        self.assertIn("result: DIFFERENT", self.out.getvalue())
        self.assertIn("differences at raw frames 9..14", self.out.getvalue())

    def test_single_state_record_difference(self):
        self.assertEqual(self.jrun({"tweak": {"restore-state": {25: 1}}}), 1)
        r = self.report()["streams"]
        self.assertEqual((r["restore-state.bin"]["first_difference_frame"],
                          r["restore-state.bin"]["last_difference_frame"]), (25, 25))
        self.assertIsNone(r["title-state.bin"]["first_difference_frame"])

    # ---- omission / coverage
    COMMON = {"startup": {6}, "title-state": {15}, "restore-state": {25}, "game-mode": {6, 15, 25}}

    def test_common_interior_omission_is_incomplete_not_identical(self):
        # review probe (a): the same frames missing from every stream on both sides used to return 0
        code = self.jrun({"omit": self.COMMON}, {"omit": self.COMMON})
        self.assertEqual(code, 4)
        r = self.report()
        self.assertEqual(r["verdict"], "INCOMPLETE")
        for name, s in r["streams"].items():
            self.assertIsNone(s["first_difference_frame"], name)  # the runs agree; only coverage fails
        self.assertEqual(r["streams"]["title-state.bin"]["coverage"]["a"]["missing"], "15")
        self.assertEqual(r["streams"]["game-mode.bin"]["coverage"]["b"]["missing"], "6,15,25")
        self.assertIn("result: INCOMPLETE", self.out.getvalue())

    def test_common_tail_omission_is_incomplete(self):
        tail = {"startup": {9}, "title-state": {19}, "restore-state": {39, 40}, "game-mode": {39, 40}}
        self.assertEqual(self.jrun({"omit": tail}, {"omit": tail}), 4)
        r = self.report()
        self.assertEqual(r["streams"]["restore-state.bin"]["coverage"]["a"]["missing"], "39-40")
        self.assertEqual(r["streams"]["startup.bin"]["coverage"]["b"]["missing"], "9")

    def test_asymmetric_omission_is_incomplete(self):
        self.assertEqual(self.jrun({}, {"omit": {"title-state": {15}}}), 4)
        t = self.report()["streams"]["title-state.bin"]
        self.assertEqual(t["coverage"]["a"]["missing"], "none")
        self.assertEqual(t["coverage"]["b"]["missing"], "15")
        self.assertEqual(t["first_difference_frame"], 15)  # the payload difference is still reported

    def test_record_in_inactive_gap_is_incomplete(self):
        for stream, gap in (("title-state", 21), ("startup", 10), ("restore-state", 21)):
            with self.subTest(stream=stream):
                self.clean()
                self.assertEqual(self.jrun({"extra": {stream: {gap}}}), 4)
                c = self.report()["streams"][stream + ".bin"]["coverage"]["a"]
                self.assertEqual((c["unexpected"], c["missing"]), (str(gap), "none"))

    def test_restore_cancel_gap_between_title_intervals(self):
        self.assertEqual(self.jrun({"extra": {"title-state": {25}}}, route="restore-cancel"), 4)
        self.assertEqual(self.report()["streams"]["title-state.bin"]["coverage"]["a"]["unexpected"], "25")

    def test_missing_successor_event_malformed(self):
        for route, ev in (("restore", "Native Restore constructed"), ("newgame", "Native name entry constructed"),
                          ("restore-cancel", "Native Restore retired")):
            with self.subTest(route=route):
                self.clean()
                kw = {"drop_events": (ev,)}
                self.assertEqual(self.run_pair(kw, kw, route=route), 2)
                self.assertIn("not reported in order", self.out.getvalue())

    def test_all_empty_active_stream_is_incomplete(self):
        kw = {"empty": ("title-state",)}
        self.assertEqual(self.jrun(kw, kw), 4)
        c = self.report()["streams"]["title-state.bin"]["coverage"]["a"]
        self.assertEqual((c["expected"], c["missing"]), ("11-19", "11-19"))

    def test_empty_stream_without_events_is_fine(self):
        kw = {"empty": ("name-entry-state",)}
        self.assertEqual(self.run_pair(kw, kw), 0)

    # ---- anchors
    def test_shifted_anchors_identical_pattern_is_anchors_differ(self):
        self.assertEqual(self.jrun({}, {"late": 1}), 3)
        out = self.out.getvalue()
        self.assertIn("result: ANCHORS-DIFFER", out)
        self.assertIn("not attributed to timing", out)
        self.assertNotIn("TIMING-DIFFERENT", out)
        r = self.report()
        self.assertFalse(r["anchors_equal"])
        self.assertEqual(r["incomplete"], [])
        self.assertEqual(r["streams"]["title-state.bin"]["coverage"]["b"]["expected"], "12-20")
        self.assertIsNone(r["streams"]["game-mode.bin"]["first_difference_frame"])

    def test_shifted_anchors_never_identical_on_any_route(self):
        # review probe (b): a shifted run must not be IDENTICAL nor described as timing-explained
        for route in ("restore", "newgame", "restore-cancel"):
            with self.subTest(route=route):
                self.clean()
                self.assertEqual(self.jrun({}, {"late": 2}, route=route), 3)
                self.assertNotIn("result: IDENTICAL", self.out.getvalue())
                self.assertIn("not attributed to timing", self.out.getvalue())

    def test_shifted_anchors_plus_corrupted_payload_reports_payload(self):
        b = {"late": 1, "tweak": {"game-mode": {17: 1}, "title-state": {15: 1}}}
        self.assertEqual(self.jrun({}, b), 3)
        out = self.out.getvalue()
        self.assertIn("not attributed to timing", out)
        self.assertNotIn("result: IDENTICAL", out)
        r = self.report()
        self.assertEqual(r["verdict"], "ANCHORS-DIFFER")
        self.assertTrue(r["payload_differs"])
        g = r["streams"]["game-mode.bin"]
        self.assertEqual((g["first_difference_frame"], g["last_difference_frame"]), (17, 17))
        t = r["streams"]["title-state.bin"]
        self.assertIsNotNone(t["first_difference_frame"])
        self.assertLessEqual(t["first_difference_frame"], 15)
        self.assertGreaterEqual(t["last_difference_frame"], 15)

    def test_shifted_anchors_with_incomplete_coverage_is_incomplete(self):
        self.assertEqual(self.jrun({}, {"late": 1, "omit": {"title-state": {15}}}), 4)

    # ---- excluded word, audio, PNG
    def test_storage_probe_word_excluded_but_word_8_counts(self):
        self.assertEqual(self.jrun({}, {"word_tweak": {"startup": {(4, 9): 1}}}), 0)
        self.assertIn("word 9 excluded", self.out.getvalue())
        self.assertEqual(self.report()["streams"]["startup.bin"]["excluded_words"], [9])
        self.clean()
        self.assertEqual(self.jrun({}, {"word_tweak": {"startup": {(4, 8): 1}}}), 1)
        self.assertEqual(self.report()["streams"]["startup.bin"]["first_difference_frame"], 4)

    def test_audio_difference_frame(self):
        self.assertEqual(self.jrun({}, {"audio_tweak": 7 * PER + 2}), 1)
        au = self.report()["audio"]
        self.assertEqual((au["first_difference_frame"], au["last_difference_frame"]), (7, 7))
        self.assertEqual(au["samples_per_frame"], PER)
        self.assertIn("differences in raw frames 7..7", self.out.getvalue())

    def test_final_png_difference(self):
        self.assertEqual(self.run_pair({}, {"png": b"other"}), 1)
        self.assertIn("final PNG: different", self.out.getvalue())

    # ---- malformed inputs
    def test_copy_of_run_is_refused(self):
        a, b = self.base / "a", self.base / "b"
        make_run(a)
        shutil.copytree(a, b)
        o = io.StringIO()
        with redirect_stdout(o), redirect_stderr(o):
            self.assertEqual(rc.main([str(a), str(b), "--route", "restore"]), 2)
        self.assertIn("identical command.txt", o.getvalue())

    def test_self_comparison_malformed(self):
        self.assertEqual(self.run_pair(same=True), 2)
        self.assertIn("self comparison", self.out.getvalue())

    def test_missing_or_unknown_route_malformed(self):
        self.assertEqual(self.run_pair(route_arg=False), 2)
        self.clean()
        self.assertEqual(self.run_pair(route_arg=False, extra=["--route", "bogus"]), 2)

    def test_different_frames_malformed(self):
        self.assertEqual(self.run_pair({}, {"frames": N + 1}), 2)
        self.assertIn("different --frames", self.out.getvalue())

    def test_nonzero_or_missing_exit_malformed(self):
        self.assertEqual(self.run_pair({}, {"exit_text": "139"}), 2)
        self.assertIn("exit status is missing or not 0", self.out.getvalue())
        self.clean()
        self.assertEqual(self.run_pair({"exit_text": None}, {"exit_text": None}), 2)

    def test_missing_stream_file_malformed(self):
        self.assertEqual(self.run_pair({}, {"skip": ("name-entry-state",)}), 2)
        self.assertIn("missing name-entry-state.bin", self.out.getvalue())

    def test_non_whole_record_file_malformed(self):
        self.assertEqual(self.run_pair({"partial": ("title-state",)}), 2)
        self.assertIn("not a whole number", self.out.getvalue())

    def test_duplicate_frame_malformed(self):
        self.assertEqual(self.run_pair({"dup": "game-mode"}), 2)
        self.assertIn("duplicate frame 2", self.out.getvalue())

    def test_audio_length_mismatch_malformed(self):
        self.assertEqual(self.run_pair({}, {"audio_len": N * PER + N}), 2)
        self.assertIn("audio streams differ in length", self.out.getvalue())

    def test_audio_not_whole_frames_malformed(self):
        self.assertEqual(self.run_pair({"audio_len": N * PER + 1}, {"audio_len": N * PER + 1}), 2)

    def test_empty_anchors_malformed(self):
        self.assertEqual(self.run_pair({"no_events": True}, {"no_events": True}), 2)
        self.assertIn("no source-event anchors", self.out.getvalue())

if __name__ == "__main__":
    unittest.main()
