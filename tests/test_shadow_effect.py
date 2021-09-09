import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_shadow_effect import compare_state,compare_rings

class ShadowEffectTests(unittest.TestCase):
    def test_lifetime_and_state_changes(self):
        rows=[]
        for step in range(84):
            r=[0]*381;r[:7]=[step,0,24,0 if step<2 else 0x20 if step<82 else 0x120,
                            8,80,max(1,min(step,81))]
            rows.append(struct.pack('<381I',*r))
        data=b''.join(rows)
        self.assertEqual(compare_state(data,data),32004)
        for word in (2,3,4,5,6,381*2+7,381*83+28):
            bad=bytearray(data);bad[word*4]^=1
            with self.assertRaises(ValueError):compare_state(data,bad)
        with self.assertRaises(ValueError):compare_state(data[:-4],data[:-4])
        bad=bytearray(data);bad[381*82*4+12]^=1
        with self.assertRaisesRegex(ValueError,'lifetime'):compare_state(bad,bad)

    def test_ring_identity_tags_extent_and_values(self):
        source=bytearray();native=bytearray()
        for step in range(84):
            for element in range(3):
                source+=struct.pack('<4I',step,element,0,60)
                for i in range(60):source+=struct.pack('<3I',0,0,1+i%2)
                native+=struct.pack('<33I',step,element,30,*([0]*30))
        self.assertEqual(compare_rings(source,native),7560)
        for word in (0,1,2,3,32):
            bad=bytearray(native);bad[word*4]^=1
            with self.assertRaises(ValueError):compare_rings(source,bad)
        for word in (0,1,2,3,9):
            bad=bytearray(source);bad[word*4]^=1
            with self.assertRaises(ValueError):compare_rings(bad,native)
        with self.assertRaises(ValueError):compare_rings(source,native[:-4])
        with self.assertRaises(ValueError):compare_rings(source[:-4],native)
