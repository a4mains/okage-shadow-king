import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_shadow_cache import compare,fixture

def pack(words):return struct.pack('<%dI'%len(words),*words)

class ShadowCacheTests(unittest.TestCase):
    def test_every_word_identity_and_extent(self):
        inputs=fixture()[:292]
        words=[0,1,0]+[0]*48
        output=pack(words)
        self.assertEqual(compare(output,output,inputs)['cache_vertices_exact'],2)
        for i in range(len(words)):
            changed=words.copy();changed[i]^=1
            with self.subTest(word=i):
                with self.assertRaises(ValueError):compare(output,pack(changed),inputs)
        for bad in (b'',output[:-4],output+pack([0])):
            with self.assertRaises(ValueError):compare(bad,bad,inputs)
        for bad in (b'',inputs[:-4],inputs*129,inputs*2,fixture()[:584]):
            with self.assertRaises(ValueError):compare(output,output,bad)
        for at,value in ((0,1),(1,7),(2,0x7fc00000)):
            bad=bytearray(inputs);struct.pack_into('<I',bad,at*4,value)
            with self.assertRaises(ValueError):compare(output,output,bad)

    def test_clip_aggregation_and_invalid_flags(self):
        inputs=fixture()[:292];words=[0,1,0]+[0]*48
        words[38]=1;words[2]=1
        self.assertEqual(compare(pack(words),pack(words),inputs)['clip_status_counts'][1],1)
        words[46]=1;words[2]=0xffffffff
        self.assertEqual(compare(pack(words),pack(words),inputs)['clip_status_counts'][-1],1)
        words[2]=0
        with self.assertRaisesRegex(ValueError,'aggregation'):compare(pack(words),pack(words),inputs)
        words[38]=64
        with self.assertRaisesRegex(ValueError,'flags'):compare(pack(words),pack(words),inputs)
