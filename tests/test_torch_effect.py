import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_torch_effect import compare_quads
from verify_map_effect import compare

class TorchAuditTests(unittest.TestCase):
    def fixture(self):
        words = []
        for step in range(3):
            row = [0] * 40
            row[0] = step
            for i in range(3):
                row[1 + i * 13] = i
            words.extend(row)
        return struct.pack('<120I', *words)

    def test_quad_identity_extent_and_each_coordinate(self):
        expected = self.fixture()
        self.assertEqual(compare_quads(expected, expected, 1)['records'], 3)
        for at in range(120):
            bad = bytearray(expected)
            bad[at * 4] ^= 1
            with self.assertRaises(ValueError):
                compare_quads(expected, bad, 1)
        for data in (expected[:-1], expected + b'\0'):
            with self.assertRaises(ValueError):
                compare_quads(expected, data, 1)

    def test_original_identity_and_order_are_checked(self):
        expected = self.fixture()
        for at in (0, 1, 14, 27, 40, 80):
            bad = bytearray(expected)
            bad[at * 4] ^= 1
            with self.assertRaises(ValueError):
                compare_quads(bad, bad, 1)

    def test_state_has_no_camera_float_tolerance(self):
        rows = []
        for step in range(3):
            row = [0] * 205
            row[:5] = [step, 0, 91, 0x8000, 4]
            rows.extend(row)
        expected = struct.pack('<615I', *rows)
        compare(expected, expected, 91, 4, 1)
        for index in (7, 29 + 24, 29 + 32, 29 + 3 * 44 + 24):
            bad = bytearray(expected)
            bad[(410 + index) * 4] ^= 1
            with self.assertRaises(ValueError):
                compare(expected, bad, 91, 4, 1)
