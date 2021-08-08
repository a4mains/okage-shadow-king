import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_summon_effects import LAYOUT, compare_ring
from verify_map_effect import compare

class SummoningAuditTests(unittest.TestCase):
    def fixture(self):
        original = bytearray()
        native = bytearray()
        for step in range(2):
            for element, piece, count in LAYOUT:
                original += struct.pack('<4I', step, element, piece, count)
                for vertex in range(count):
                    tag = 1 + vertex % 2 if element == 1 else 1
                    original += struct.pack('<3I', (65500 - step) << 16, 0, tag)
            native += struct.pack('<33I', step, 1, 30, *([65500 - step] * 30))
        return original, native

    def test_ring_exact(self):
        original, native = self.fixture()
        self.assertEqual(compare_ring(original, native, 0)['mutable_y_words_exact'], 60)

    def test_ring_rejects_changed_identity_coordinate_or_extent(self):
        original, native = self.fixture()
        for offset in (0, 4, 8, 12, len(native) - 4):
            bad = bytearray(native)
            bad[offset] ^= 1
            with self.assertRaises(ValueError):
                compare_ring(original, bad, 0)
        for ref, got in ((original[:-1], native), (original + b'\0', native),
                         (original, native[:-4]), (original, native + b'\0' * 4)):
            with self.assertRaises(ValueError):
                compare_ring(ref, got, 0)

    def test_ring_rejects_reference_order_and_tags(self):
        original, native = self.fixture()
        ring_offset = 16 + 17 * 12
        for offset in (0, ring_offset + 4, ring_offset + 16 + 8):
            bad = bytearray(original)
            bad[offset] ^= 1
            with self.assertRaises(ValueError):
                compare_ring(bad, native, 0)

    def test_summoning_state_has_no_float_tolerance(self):
        for template, elements in ((33, 9), (115, 5)):
            stride = 29 + 44 * elements
            rows = []
            for step in range(3):
                row = [0] * stride
                row[:5] = [step, 0, template, 0, elements]
                rows.extend(row)
            expected = struct.pack(f'<{len(rows)}I', *rows)
            compare(expected, expected, template, elements, 1)
            for index in (7, 29 + 24, 29 + 32, 29 + 43):
                bad = bytearray(expected)
                bad[(2 * stride + index) * 4] ^= 1
                with self.assertRaises(ValueError):
                    compare(expected, bad, template, elements, 1)
