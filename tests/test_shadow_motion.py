import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_shadow_motion import compare

def pack(words):return struct.pack('<%dI'%len(words),*words)

class ShadowMotionTests(unittest.TestCase):
    def test_every_word_and_extent(self):
        words=[1,1,0,1,0,0,0]+[0]*20;data=pack(words)
        self.assertEqual(compare(data,data,0)['positions_exact'],1)
        for i in range(len(words)):
            bad=words.copy();bad[i]^=1
            with self.assertRaises(ValueError):compare(data,pack(bad),0)
        for bad in (b'',data[:-4],data+pack([0])):
            with self.assertRaises(ValueError):compare(bad,bad,0)
        for at,value in ((0,0),(1,0),(2,241),(3,32769),(4,1)):
            bad=words.copy();bad[at]=value
            with self.assertRaises(ValueError):compare(pack(bad),pack(bad),0)
        with self.assertRaises(ValueError):compare(data,data,1)
