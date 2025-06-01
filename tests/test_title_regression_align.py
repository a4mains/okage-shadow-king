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

def make_run(d, logo, title, title_until=FRAMES, audio_value=0, startup_tweak=None):
    d.mkdir()
    (d / "command.txt").write_text(f"okage-native --frames {FRAMES} --capture f.png\n")
    (d / "stdout.txt").write_text(f"Native startup constructed selector=0 frame={logo}\nNative title constructed frame={title}\n")
    s = array("I")
    for f in range(2, title + 1):
        rec = [f, 0 if f >= logo else 3, max(0, f - logo), 0, 0, 0, 1, 0, 0, f * 7, 0, 0x412c25e1]
        if startup_tweak and f == startup_tweak:
            rec[2] += 1
        s.extend(rec)
    (d / "f.png.startup.bin").write_bytes(s.tobytes())
    t = array("I")
    for f in range(title + 1, title_until + 1):
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
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return al.main([str(a), str(b)])

    def test_offset_runs_align_identical(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20), dict(logo=7, title=22)), 0)

    def test_logo_difference_detected(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20), dict(logo=7, title=22, startup_tweak=12)), 1)

    def test_audio_difference_detected(self):
        self.assertEqual(self.run_pair(dict(logo=5, title=20), dict(logo=5, title=20, audio_value=1)), 1)

    def test_truncated_title_records_are_malformed_or_different(self):
        code = self.run_pair(dict(logo=5, title=20), dict(logo=5, title=20, title_until=22))
        self.assertIn(code, (1, 2))
        self.assertNotEqual(code, 0)

if __name__ == "__main__":
    unittest.main()
