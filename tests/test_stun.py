import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from verify_stun import audit_follow, subtract_toward_zero

class StunTraceTests(unittest.TestCase):
    def test_follow_identity_and_pose_corruption(self):
        one = 0x3f800000
        rotation = [0, 0, 0, one]
        position = [0, 0xbc23d70a, 0, one]
        row = [1, 1, 0, 0, 0, 0xffffffff, 0, 4, 0, one]
        row += rotation + position + [one]*4 + rotation + [0, 0, 0, one]
        data = struct.pack('<30I', *row)
        self.assertEqual(audit_follow(data), dict(frames=1, movement_frozen_follow_frames=1))
        for index in (0, 1, 2, 8, 10, 15, 18, 22, 27):
            broken = row.copy()
            broken[index] ^= 1
            if index == 1:
                broken[index] = 2
            with self.assertRaises(ValueError):
                audit_follow(struct.pack('<30I', *broken))
        with self.assertRaises(ValueError):
            audit_follow(data[:-1])

    def test_subtraction_rounding_direction(self):
        amount = struct.unpack('<f', struct.pack('<I', 0x3c23d70a))[0]
        self.assertEqual(subtract_toward_zero(0, amount), 0xbc23d70a)
        # 1-.01 is below the nearest float: toward-zero chooses its predecessor.
        self.assertEqual(subtract_toward_zero(0x3f800000, amount), 0x3f7d70a3)
        self.assertEqual(subtract_toward_zero(0xbf800000, amount), 0xbf8147ae)

if __name__ == '__main__':
    unittest.main()
