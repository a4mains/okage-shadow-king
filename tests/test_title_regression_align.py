"""Negative controls for tools/title_regression_align.py (synthetic runs)."""
import io
import sys
import tempfile
import unittest
from array import array
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import title_regression_align as al  # noqa: E402

FRAMES, PER = 40, 10

def make_run(d, logo, title, title_until=FRAMES, audio_value=0, startup_tweak=None, drop_startup=(),
             drop_title=(), extra_startup=()):
    """Startup records 2..request (request = title-3), none in the two transition frames, title from title."""
    request = title - 3
    d.mkdir()
    (d / "command.txt").write_text(f"okage-native --frames {FRAMES} --capture f.png\n")
    (d / "stdout.txt").write_text(f"Native startup constructed selector=0 frame={logo}\n"
                                  f"Native startup requested selector=1 frame={request}\n"
                                  f"Native title constructed frame={title}\n")
    s = array("I")
    for f in sorted({*range(2, request + 1), *extra_startup}):
        if f in drop_startup:
            continue
        rec = [f, 0 if f >= logo else 3, max(0, f - logo), 0, 0, 0, 1, 0, 0, f * 7, 0, 0x412c25e1]
        if startup_tweak and f == startup_tweak:
            rec[2] += 1
        s.extend(rec)
    (d / "f.png.startup.bin").write_bytes(s.tobytes())
    t = array("I")
    for f in range(title, title_until + 1):
        if f not in drop_title:
            t.extend([f, 1, f - title, 9])
    (d / "f.png.title-state.bin").write_bytes(t.tobytes())
    a = array("h", [0] * (FRAMES * PER))
    for i in range(title * PER, FRAMES * PER):
        a[i] = (i - title * PER) % 100 + audio_value
    (d / "f.png.audio.pcm").write_bytes(a.tobytes())

class AlignControls(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)

    def run_pair(self, a_kw, b_kw):
        a, b = self.base / "a", self.base / "b"
        make_run(a, **a_kw)
        make_run(b, **b_kw)
        self.out = io.StringIO()
        with redirect_stdout(self.out), redirect_stderr(self.out):
            return al.main([str(a), str(b)])

    def test_offset_runs_align_identical(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20), dict(logo=7, title=22)), 0)
        self.assertIn("transition: 2 frames", self.out.getvalue())
        self.assertIn("all 13 recorded on both sides", self.out.getvalue())  # logo 5..17

    def test_logo_difference_detected(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20), dict(logo=7, title=22, startup_tweak=12)), 1)

    def test_audio_difference_detected(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20), dict(logo=5, title=20, audio_value=1)), 1)

    def test_truncated_title_records_are_malformed(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20), dict(logo=5, title=20, title_until=22)), 2)

    # The Title tool accepted these (exit 0, ALIGNED-IDENTICAL); they must now fail.
    def test_common_interior_logo_omission_is_malformed(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20, drop_startup={10}),
                                       dict(logo=5, title=20, drop_startup={10})), 2)
        self.assertIn("1 of 13 expected frames have no record: 10", self.out.getvalue())

    def test_common_trailing_logo_omission_is_malformed(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20, drop_startup={16, 17}),
                                       dict(logo=5, title=20, drop_startup={16, 17})), 2)

    def test_asymmetric_logo_omission_is_malformed(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20), dict(logo=5, title=20, drop_startup={9})), 2)

    def test_common_interior_title_omission_is_malformed(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20, drop_title={30}),
                                       dict(logo=5, title=20, drop_title={30})), 2)

    def test_common_trailing_title_omission_is_malformed(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20, title_until=38),
                                       dict(logo=5, title=20, title_until=38)), 2)

    def test_asymmetric_title_omission_is_malformed(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20, drop_title={25}), dict(logo=5, title=20)), 2)

    def test_record_in_transition_is_different(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20), dict(logo=5, title=20, extra_startup={18})), 1)

    def test_self_comparison_is_rejected(self):
        a = self.base / "a"
        make_run(a, logo=5, title=20)
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(al.main([str(a), str(a)]), 2)

if __name__ == "__main__":
    unittest.main()
