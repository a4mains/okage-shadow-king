import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_shadow_binding import compare

def pack(words):return struct.pack('<%dI'%len(words),*words)

class ShadowBindingTests(unittest.TestCase):
    def test_all_indices_and_matrix_words(self):
        words=[2,0,1]+[0]*32;output=pack(words)
        self.assertEqual(compare(output,output)['rest_matrices_exact'],2)
        for i in range(len(words)):
            bad=words.copy();bad[i]^=1
            with self.assertRaises(ValueError):compare(output,pack(bad))
        bad=words.copy();bad[2]=2
        with self.assertRaisesRegex(ValueError,'index'):compare(pack(bad),pack(bad))

    def test_extent(self):
        output=pack([1,0]+[0]*16)
        for bad in (b'',pack([0]),pack([4097]),output[:-4],output+pack([0])):
            with self.assertRaises(ValueError):compare(bad,bad)
