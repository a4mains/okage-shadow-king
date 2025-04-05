"""Controls for tools/startup_compare.py (needs the ignored Startup evidence under local/research)."""
import io
import json
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import startup_compare as cmp  # noqa: E402

ORIG = ROOT / "local/research/startup-original-v1/analysis"
NATIVE = ROOT / "local/research/startup-session-native-v1/report1"

@unittest.skipUnless(ORIG.is_dir() and NATIVE.is_dir(), "Startup startup evidence absent")
class StartupCompareTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.nat = Path(self.tmp.name) / "native"
        shutil.copytree(NATIVE, self.nat)
        self.orig = Path(self.tmp.name) / "orig"
        shutil.copytree(ORIG, self.orig)

    def run_cmp(self):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = cmp.main([str(self.orig), str(self.nat)])
        return code, out.getvalue(), err.getvalue()

    def state(self):
        return json.loads((self.nat / "state.json").read_text())

    def put_state(self, s):
        (self.nat / "state.json").write_text(json.dumps(s))

    def test_real_native_state_matches_original(self):
        code, out, _ = self.run_cmp()
        self.assertEqual(code, 0, out)
        self.assertIn("result: IDENTICAL", out)

    def test_vram_mask_and_value_negatives(self):
        known = bytearray((self.nat / "vram.known").read_bytes())
        idx = next(i for i, b in enumerate(known) if b == 3)
        for name, mutate in {
            "dropped lane": lambda k, v: k.__setitem__(idx, 1),
            "extra lane": lambda k, v: k.__setitem__(next(i for i, b in enumerate(k) if b == 0), 2),
        }.items():
            with self.subTest(name):
                k, v = bytearray(known), bytearray((self.nat / "vram.bytes").read_bytes())
                mutate(k, v)
                (self.nat / "vram.known").write_bytes(k)
                (self.nat / "vram.bytes").write_bytes(v)
                self.assertEqual(self.run_cmp()[0], 1)
                shutil.copy(NATIVE / "vram.known", self.nat / "vram.known")
        v = bytearray((self.nat / "vram.bytes").read_bytes())
        v[idx] ^= 0x01
        (self.nat / "vram.bytes").write_bytes(v)
        self.assertEqual(self.run_cmp()[0], 1)

    def test_unknown_nibble_value_is_not_compared_but_known_one_is(self):
        # A flipped bit inside an UNKNOWN nibble of the native image must not matter...
        known = (self.nat / "vram.known").read_bytes()
        idx = next(i for i, b in enumerate(known) if b == 1)
        v = bytearray((self.nat / "vram.bytes").read_bytes())
        v[idx] ^= 0xf0
        (self.nat / "vram.bytes").write_bytes(v)
        self.assertEqual(self.run_cmp()[0], 0)

    def test_state_negatives(self):
        def tweak(fn):
            s = self.state()
            fn(s)
            self.put_state(s)
            code = self.run_cmp()[0]
            shutil.copy(NATIVE / "state.json", self.nat / "state.json")
            return code
        self.assertEqual(tweak(lambda s: s["allocations"][2].update(references=2)), 1)
        self.assertEqual(tweak(lambda s: s["allocations"][5].update(tbp=s["allocations"][5]["tbp"] + 1)), 1)
        self.assertEqual(tweak(lambda s: s["cursors"].__setitem__(0, s["cursors"][0] + 1)), 1)
        self.assertEqual(tweak(lambda s: (s["handles"][3].update(record=4), s["handles"][4].update(record=3))), 1)
        self.assertEqual(tweak(lambda s: s["handles"][3].update(record=9)), 2)  # record without its handle: malformed
        self.assertEqual(tweak(lambda s: s.update(free_extras=s["free_extras"] + 1)), 1)
        self.assertEqual(tweak(lambda s: s["records"][0].update(hash_slot=5)), 1)
        self.assertEqual(tweak(lambda s: s.update(reserved=[1])), 1)

    def test_empty_handmade_and_unknown_key_inputs_are_malformed(self):
        s = self.state()
        s["unexpected"] = 1
        self.put_state(s)
        self.assertEqual(self.run_cmp()[0], 2)
        shutil.copy(NATIVE / "state.json", self.nat / "state.json")
        empty = {k: ([] if isinstance(v, list) else v) for k, v in self.state().items()}
        empty["cursors"] = [0] * 64
        self.put_state(empty)
        self.assertEqual(self.run_cmp()[0], 2)  # not 79 allocations / 36 handles
        shutil.copy(NATIVE / "state.json", self.nat / "state.json")
        # An original that is not the pinned capture is refused even if self-consistent.
        text = (self.orig / "final-state.jsonl").read_text().replace('"free_records": 476', '"free_records": 475')
        (self.orig / "final-state.jsonl").chmod(0o644)
        (self.orig / "final-state.jsonl").write_text(text)
        self.assertEqual(self.run_cmp()[0], 2)

    def test_malformed_is_exit_2(self):
        s = self.state()
        del s["cursors"]
        self.put_state(s)
        self.assertEqual(self.run_cmp()[0], 2)
        shutil.copy(NATIVE / "state.json", self.nat / "state.json")
        s = self.state()
        s["coverage"] = "not-run"
        self.put_state(s)
        self.assertEqual(self.run_cmp()[0], 2)
        shutil.copy(NATIVE / "state.json", self.nat / "state.json")
        text = (self.nat / "state.json").read_text().replace('"available"', '"available": 1, "available"', 1)
        (self.nat / "state.json").write_text(text)
        self.assertEqual(self.run_cmp()[0], 2)  # duplicate key
        shutil.copy(NATIVE / "state.json", self.nat / "state.json")
        (self.nat / "vram.known").write_bytes(b"\0" * 100)
        self.assertEqual(self.run_cmp()[0], 2)  # short image
        self.assertEqual(cmp.main([]), 2)

if __name__ == "__main__":
    unittest.main()
