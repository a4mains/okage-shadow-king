import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_ordinary_visibility import compare, fixtures, normalize_original

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

    def test_source_sort_continuations_are_recorded_and_normalized(self):
        cases = [dict(id=1, culling=1, blocks=[[]]),
                 dict(id=2, culling=1, blocks=[[], []]),
                 dict(id=3, culling=1, blocks=[])]
        raw = struct.pack('<13I', 1, 1, 0x1863b8, 0, 0x41200000,
                          2, 1, 0x1863b0, 1, 0x41200000,
                          3, 0, 0xffffffff)
        normalized, stops, histogram = normalize_original(raw, cases)
        self.assertEqual(struct.unpack('<10I', normalized),
                         (1, 1, 0, 0x41200000, 2, 1, 1, 0x41200000, 3, 0))
        self.assertEqual([row['stop_pc'] for row in stops],
                         ['0x001863b8', '0x001863b0', None])
        self.assertEqual(histogram, {'0x001863b0': 1, '0x001863b8': 1})

    def test_unexpected_sort_stop_and_empty_sort_are_rejected(self):
        cases = [dict(id=1, culling=1, blocks=[[]])]
        with self.assertRaises(ValueError):
            normalize_original(struct.pack('<3I', 1, 1, 0x1863c0), cases)
        with self.assertRaises(ValueError):
            normalize_original(struct.pack('<3I', 1, 0, 0x1863b0),
                               [dict(id=1, culling=1, blocks=[])])

if __name__ == '__main__':
    unittest.main()
