import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_ordinary_visibility import compare, fixtures

class OrdinaryVisibilityTests(unittest.TestCase):
    def test_fixtures_are_bounded_and_force_culling(self):
        data, cases = fixtures()
        again, other = fixtures()
        self.assertEqual(data, again)
        self.assertEqual(cases, other)
        self.assertEqual(len(cases), 90)
        self.assertTrue(all(case['culling'] == 1 for case in cases))
        self.assertEqual(struct.unpack_from('<I', data, 4)[0], 1)

    def test_output_extent_and_stable_depth_order(self):
        cases = [dict(id=7, culling=1, blocks=[[], [], []])]
        output = struct.pack('<8I', 7, 3, 0, 0x412e0000, 1, 0x41220000,
                             2, 0x41220000)
        self.assertEqual(compare(output, output, cases)['selected_blocks_exact'], 3)
        wrong_order = struct.pack('<8I', 7, 3, 1, 0x41220000, 0, 0x412e0000,
                                  2, 0x41220000)
        with self.assertRaises(ValueError):
            compare(wrong_order, wrong_order, cases)
        with self.assertRaises(ValueError):
            compare(output, output[:-4], cases)

if __name__ == '__main__':
    unittest.main()
