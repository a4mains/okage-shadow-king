import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_shadow_bounds import compare

class ShadowBoundTests(unittest.TestCase):
    def test_every_word_and_malformed_records(self):
        bound=struct.pack('<5fI',1,2,3,1,4,0)
        data=struct.pack('<2I',1,1)+bound+struct.pack('<I',1)+bound
        self.assertEqual(compare(data,data)['bounds_exact'],2)
        for i in range(0,len(data),4):
            bad=bytearray(data);bad[i]^=1
            with self.assertRaises(ValueError):compare(data,bad)
        for bad in [b'',data[:-4],data+bytes(4),bytes(4)]:
            with self.assertRaises(ValueError):compare(bad,bad)
        for offset,value in [(4,0),(4,4097),(20,0),(24,0xbf800000),(28,4096),(8,0x7fc00000)]:
            bad=bytearray(data);struct.pack_into('<I',bad,offset,value)
            with self.assertRaises(ValueError):compare(bad,bad)
