import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_church_basement_effects import compare_uv
from verify_map_effect import compare

class BasementEffectVerifierTests(unittest.TestCase):
    def test_uv_requires_complete_ordered_tagged_records(self):
        original = bytearray()
        native = bytearray()
        for step in range(2):
            for element in range(16):
                original += struct.pack('<12I', step, element, 0, 4, 11, 0, 22, 0, 33, 0, 44, 0)
                native += struct.pack('<7I', step, element, 4, 11, 22, 33, 44)
        self.assertEqual(compare_uv(original, native, 0), 224)
        for source, result in [(original[:-1], native), (original + bytes(4), native),
                               (original, native[:-4]), (original, native + bytes(4))]:
            with self.assertRaises(ValueError):
                compare_uv(source, result, 0)
        for index in (0, 4, 8, 12, 20):
            changed = bytearray(original)
            changed[index] ^= 1
            with self.assertRaises(ValueError):
                compare_uv(changed, native, 0)

    def test_new_templates_never_inherit_sine_tolerance(self):
        for template, elements in ((90, 1), (105, 16)):
            stride = 29 + elements * 44
            words = []
            for step in range(3):
                words.extend([step, 0, template, 0, elements] + [0] * (stride - 5))
            expected = struct.pack('<' + 'I' * len(words), *words)
            compare(expected, expected, template, elements, 1)
            words[2 * stride + 29 + 32] = 1
            with self.assertRaises(ValueError):
                compare(expected, struct.pack('<' + 'I' * len(words), *words), template, elements, 1)

if __name__ == '__main__':
    unittest.main()
