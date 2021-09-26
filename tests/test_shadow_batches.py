import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_shadow_batches import compare

def pack(words):return struct.pack('<%dI'%len(words),*words)

class ShadowBatchTests(unittest.TestCase):
    def test_all_words_and_bounds(self):
        words=[1,1,3,1,10,11,12,3,0,1,2,0];data=pack(words)
        self.assertEqual(compare(data,data)['corners_exact'],3)
        for i in range(len(words)):
            bad=words.copy();bad[i]^=1
            with self.assertRaises(ValueError):compare(data,pack(bad))
        for bad in (b'',data[:-4],data+pack([0])):
            with self.assertRaises(ValueError):compare(bad,bad)
        for at,value in ((0,0),(1,4097),(2,98),(3,98),(5,10),(7,5),(8,1),(10,3)):
            bad=words.copy();bad[at]=value
            with self.assertRaises(ValueError):compare(pack(bad),pack(bad))

    def test_empty_material_passes(self):
        data=pack([1,0,0]);self.assertEqual(compare(data,data)['batches'],0)
