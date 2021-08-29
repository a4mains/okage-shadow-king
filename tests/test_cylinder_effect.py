import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from verify_cylinder_effect import compare_uv

class CylinderUVTests(unittest.TestCase):
    def fixture(self):
        words = []
        for step in range(3):
            for element in range(2):
                words += [step, element, 0, 34]
                for vertex in range(17):
                    words += [vertex | (2080 << 16), 0, vertex | ((305+step) << 16), 2]
        return struct.pack('<'+str(len(words))+'I', *words)

    def test_mutation_and_truncation(self):
        original = self.fixture()
        self.assertEqual(compare_uv(original, original, 3), 204)
        for at in range(len(original)):
            changed = bytearray(original);changed[at] ^= 1
            with self.assertRaises(ValueError):
                compare_uv(original, changed, 3)
        with self.assertRaises(ValueError):
            compare_uv(original[:-4], original[:-4], 3)

    def test_reject_bad_original_layout_and_sharing(self):
        for at in [0, 4, 8, 12, 20, 28, 72*4+16]:
            changed = bytearray(self.fixture());changed[at] ^= 1
            with self.assertRaises(ValueError):
                compare_uv(changed, changed, 3)

if __name__ == '__main__':
    unittest.main()
