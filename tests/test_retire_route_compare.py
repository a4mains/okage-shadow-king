"""Negative controls for tools/retire_route_compare.py (synthetic runs)."""
import io
import json
import sys
import tempfile
import unittest
from array import array
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import retire_route_compare as rc  # noqa: E402

FRAMES, PER = 20, 4
OTHERS = ("title-state", "restore-state", "name-entry-state", "game-mode")
RECLEN = 5
ANCHORS = (("Native title constructed", 5), ("Native game-mode entered", 12))

def make_run(d, frames=FRAMES, anchors=ANCHORS, tweak=None, drop=None, omit=(), audio_tweak=None,
             png=b"png", exit_text="0", skip=(), partial=(), dup=None, audio_len=None, empty=()):
    """tweak/drop: {suffix: frame}; omit: frames omitted from every stream; records cover frames 2..frames-1."""
    d.mkdir()
    (d / "command.txt").write_text(f"okage-native --frames {frames} --capture {d}/f.png\n")
    (d / "stdout.txt").write_text("".join(f"{t} frame={f}\n" for t, f in anchors) or "nothing here\n")
    if exit_text is not None:
        (d / "exit.txt").write_text(exit_text + "\n")
    (d / "f.png").write_bytes(png)
    tweak, drop = tweak or {}, drop or {}
    for suffix, length in [("startup", 12)] + [(o, RECLEN) for o in OTHERS]:
        w = array("I")
        if suffix not in empty:
            for f in range(2, frames):
                if f in omit or drop.get(suffix) == f:
                    continue
                rec = [f] + [f * 3 + k for k in range(length - 1)]
                if tweak.get(suffix) == f:
                    rec[-1] += 1
                w.extend(rec)
            if dup == suffix:
                w.extend([2] + [0] * (length - 1))
            if suffix in partial:
                w.append(7)
        if suffix not in skip:
            (d / f"f.png.{suffix}.bin").write_bytes(w.tobytes())
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

    def run_pair(self, a_kw=None, b_kw=None, extra=(), same=False):
        a, b = self.base / "a", self.base / "b"
        make_run(a, **(a_kw or {}))
        if same:
            b = a
        else:
            make_run(b, **(b_kw or {}))
        self.out = io.StringIO()
        with redirect_stdout(self.out), redirect_stderr(self.out):
            return rc.main([str(a), str(b), *extra])

    def report(self):
        return json.loads(self.json.read_text())

    def test_identical(self):
        self.assertEqual(self.run_pair(), 0)
        self.assertIn("result: IDENTICAL", self.out.getvalue())

    def test_record_value_difference_reports_exact_frames(self):
        code = self.run_pair({"tweak": {"game-mode": 9}}, {"tweak": {"game-mode": 14}},
                             extra=("--json", str(self.json)))
        self.assertEqual(code, 1)
        s = self.report()["streams"]["game-mode.bin"]
        self.assertEqual((s["first_difference_frame"], s["last_difference_frame"]), (9, 14))
        self.assertIn("result: DIFFERENT", self.out.getvalue())
        self.assertIn("differences at raw frames 9..14", self.out.getvalue())

    def test_single_record_difference(self):
        self.assertEqual(self.run_pair({"tweak": {"startup": 6}}), 1)
        self.assertIn("startup.bin (word 9 excluded: asynchronous storage probe): 18/18 records, frames 2-19 / 2-19, differences at raw frames 6..6",
                      self.out.getvalue())

    def test_frame_on_one_side_only(self):
        code = self.run_pair({}, {"drop": {"title-state": 8}}, extra=("--json", str(self.json)))
        self.assertEqual(code, 1)
        s = self.report()["streams"]["title-state.bin"]
        self.assertEqual((s["first_difference_frame"], s["last_difference_frame"]), (8, 8))
        self.assertEqual((s["records_a"], s["records_b"]), (18, 17))
        self.assertEqual(s["frames_a"], "2-19")
        self.assertEqual(s["frames_b"], "2-7,9-19")

    def test_common_omission_is_not_difference_but_visible(self):
        code = self.run_pair({"omit": (10,)}, {"omit": (10,)}, extra=("--json", str(self.json)))
        self.assertEqual(code, 0)
        for suffix, s in self.report()["streams"].items():
            self.assertIsNone(s["first_difference_frame"], suffix)
            self.assertEqual(s["frames_a"], "2-9,11-19", suffix)
            self.assertEqual(s["frames_b"], "2-9,11-19", suffix)
        self.assertIn("frames 2-9,11-19 / 2-9,11-19", self.out.getvalue())

    def test_anchors_differ_is_timing_different(self):
        b = {"anchors": (("Native title constructed", 6), ("Native game-mode entered", 12)),
             "tweak": {"game-mode": 9}}
        self.assertEqual(self.run_pair({}, b), 3)
        self.assertIn("result: TIMING-DIFFERENT", self.out.getvalue())
        self.assertIn("anchors differ", self.out.getvalue())

    def test_anchors_differ_alone_is_timing_different(self):
        b = {"anchors": (("Native title constructed", 6), ("Native game-mode entered", 12))}
        self.assertEqual(self.run_pair({}, b), 3)

    def test_anchors_differ_with_other_differences_still_timing(self):
        b = {"anchors": (("Native title constructed", 6),), "tweak": {"game-mode": 9}}
        self.assertEqual(self.run_pair({}, b), 3)

    def test_audio_difference_frame(self):
        code = self.run_pair({}, {"audio_tweak": 7 * PER + 2}, extra=("--json", str(self.json)))
        self.assertEqual(code, 1)
        au = self.report()["audio"]
        self.assertEqual((au["first_difference_frame"], au["last_difference_frame"]), (7, 7))
        self.assertEqual(au["samples_per_frame"], PER)
        self.assertIn("differences in raw frames 7..7", self.out.getvalue())

    def test_final_png_difference(self):
        self.assertEqual(self.run_pair({}, {"png": b"other"}), 1)
        self.assertIn("final PNG: different", self.out.getvalue())

    def test_empty_stream_both_sides_identical(self):
        self.assertEqual(self.run_pair({"empty": ("restore-state",)}, {"empty": ("restore-state",)}), 0)
        self.assertIn("0/0 records", self.out.getvalue())

    def test_empty_stream_one_side_is_difference(self):
        self.assertEqual(self.run_pair({}, {"empty": ("restore-state",)}), 1)

    def test_self_comparison_malformed(self):
        self.assertEqual(self.run_pair(same=True), 2)
        self.assertIn("self comparison", self.out.getvalue())

    def test_different_frames_malformed(self):
        self.assertEqual(self.run_pair({}, {"frames": FRAMES + 1}), 2)
        self.assertIn("different --frames", self.out.getvalue())

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
        self.assertEqual(self.run_pair({}, {"audio_len": FRAMES * PER + FRAMES}), 2)
        self.assertIn("audio streams differ in length", self.out.getvalue())

    def test_audio_not_whole_frames_malformed(self):
        self.assertEqual(self.run_pair({"audio_len": FRAMES * PER + 1}, {"audio_len": FRAMES * PER + 1}), 2)

    def test_nonzero_exit_malformed(self):
        self.assertEqual(self.run_pair({}, {"exit_text": "139"}), 2)
        self.assertIn("exit status is missing or not 0", self.out.getvalue())

    def test_storage_probe_word_excluded_but_reported(self):
        a, b = self.base / "a", self.base / "b"
        make_run(a)
        make_run(b)
        w = array("I")
        w.frombytes((b / "f.png.startup.bin").read_bytes())
        w[9] += 1
        (b / "f.png.startup.bin").write_bytes(w.tobytes())
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            self.assertEqual(rc.main([str(a), str(b)]), 0)
        self.assertIn("word 9 excluded", out.getvalue())
        w[8] += 1  # any other word still counts
        (b / "f.png.startup.bin").write_bytes(w.tobytes())
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(rc.main([str(a), str(b)]), 1)

    def test_missing_exit_file_malformed(self):
        self.assertEqual(self.run_pair({"exit_text": None}, {"exit_text": None}), 2)

    def test_copied_run_is_refused(self):
        import shutil
        a, b = self.base / "a", self.base / "b"
        make_run(a)
        shutil.copytree(a, b)
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(rc.main([str(a), str(b)]), 2)

    def test_runs_without_title_records_are_malformed(self):
        self.assertEqual(self.run_pair({"empty": ("title-state",)}, {"empty": ("title-state",)}), 2)

    def test_empty_anchors_malformed(self):
        self.assertEqual(self.run_pair({"anchors": ()}, {"anchors": ()}), 2)
        self.assertIn("no source-event anchors", self.out.getvalue())

    def test_json_matches_output(self):
        code = self.run_pair({}, {"tweak": {"startup": 4}}, extra=("--json", str(self.json)))
        self.assertEqual(code, 1)
        r = self.report()
        self.assertEqual(r["verdict"], "DIFFERENT")
        self.assertEqual(r["frames"], FRAMES)
        self.assertTrue(r["anchors_equal"])
        self.assertEqual(r["anchors_a"], [list(a) for a in ANCHORS])
        self.assertTrue(r["final_png_identical"])
        self.assertEqual(r["audio"]["samples"], FRAMES * PER)
        self.assertEqual(sorted(r["streams"]), sorted(rc.STREAMS))
        self.assertEqual(r["streams"]["startup.bin"]["first_difference_frame"], 4)
        self.assertIsNone(r["streams"]["game-mode.bin"]["first_difference_frame"])

if __name__ == "__main__":
    unittest.main()
