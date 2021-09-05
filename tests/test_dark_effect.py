import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from verify_dark_effect import compare_state
from verify_summon_drawing import packet_vertices

class DarkEffectTests(unittest.TestCase):
    def fixture(self):
        result = bytearray()
        for step in range(74):
            r = [0]*205
            r[:8] = [step,0,6,0x405 if step<2 else 0x425 if step<72 else 0x525,
                     4,70,max(1,min(step,71)),1]
            for i in range(4):r[29+i*44+40:29+i*44+44] = [0xffffffff,0,0xffffffff,0]
            result += struct.pack('<205I', *r)
        return result

    def test_lifetime_rng_and_changed_state(self):
        original = self.fixture()
        self.assertEqual(compare_state(original, original), 15170)
        for step, word in ((0,2),(2,7),(71,6),(72,3),(73,10)):
            changed = bytearray(original);changed[(step*205+word)*4] ^= 1
            with self.assertRaises(ValueError):compare_state(changed, changed)
        changed = bytearray(original);changed[1000] ^= 1
        with self.assertRaises(ValueError):compare_state(original, changed)
        with self.assertRaises(ValueError):compare_state(original[:-4], original[:-4])

    def test_reject_additive_or_depth_tested_material(self):
        for alpha, depth in ((0x48,0x30000),(0x42,0x50000)):
            header = struct.pack('<11I',0,7,0,0xffffffff,0,1,alpha,0,depth,0,1)
            with self.assertRaisesRegex(ValueError, 'material'):packet_vertices(header,6)
