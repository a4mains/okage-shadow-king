import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_shadow_assembled import compare,fixture

def pack(words):return struct.pack('<%dI'%len(words),*words)

class ShadowAssemblyTests(unittest.TestCase):
    def test_words_order_extent(self):
        inputs=fixture()[:232];words=[0,1,2]+[0]*16+[0,0,0,1]+[0]*144+[0,1,0,0];data=pack(words)
        self.assertEqual(compare(data,data,inputs,1,2)['ordered_vertices_exact'],36)
        for i in range(len(words)):
            bad=words.copy();bad[i]^=1
            with self.assertRaises(ValueError):compare(data,pack(bad),inputs,1,2)
        for bad in (b'',data[:-4],data+pack([0])):
            with self.assertRaises(ValueError):compare(bad,bad,inputs,1,2)
        for at,value in ((0,1),(1,2),(2,3),(19,1),(21,1),(22,2),(168,3)):
            bad=words.copy();bad[at]=value
            with self.assertRaises(ValueError):compare(pack(bad),pack(bad),inputs,1,2)
        for bad in (b'',inputs[:-4],inputs*33):
            with self.assertRaises(ValueError):compare(data,data,bad,1,2)
