import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "verify_animation", Path(__file__).resolve().parents[1] / "tools/verify_animation.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)

class AnimationComparisonTests(unittest.TestCase):
    def test_rejects_state_channel_pose_corruption_and_truncation(self):
        original = struct.pack("<II20f", 32, 1, *([1.0] * 20))
        with tempfile.TemporaryDirectory() as directory:
            ref, native = Path(directory) / "ref", Path(directory) / "native"
            ref.write_bytes(original)
            native.write_bytes(original)
            self.assertTrue(verifier.compare(ref, native, 1, 1, 0)["passed"])
            for at in (0, 8, 24):
                changed = bytearray(original)
                struct.pack_into("<I" if at == 0 else "<f", changed, at, 2)
                native.write_bytes(changed)
                self.assertFalse(verifier.compare(ref, native, 1, 1, 0)["passed"])
            changed = bytearray(original)
            struct.pack_into("<f", changed, 24, float("nan"))
            native.write_bytes(changed)
            self.assertFalse(verifier.compare(ref, native, 1, 1, 0)["passed"])
            native.write_bytes(original[:-1])
            with self.assertRaises(ValueError):
                verifier.compare(ref, native, 1, 1, 0)

    def test_float_tolerance_does_not_weaken_exact_channel_check(self):
        with tempfile.TemporaryDirectory() as directory:
            ref, native = Path(directory) / "ref", Path(directory) / "native"
            original = struct.pack("<II20f", 32, 1, *([1.0] * 20))
            ref.write_bytes(original)
            changed = bytearray(original)
            struct.pack_into("<f", changed, 24, 1.000001)
            native.write_bytes(changed)
            self.assertTrue(verifier.compare(ref, native, 1, 1, 0)["passed"])
            struct.pack_into("<f", changed, 8, 1.000001)
            native.write_bytes(changed)
            self.assertFalse(verifier.compare(ref, native, 1, 1, 0)["passed"])
