import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_shadow_prism import compare,fixture,TAGS

def packed(words):return struct.pack('<%dI'%len(words),*words)

class ShadowPrismTests(unittest.TestCase):
    def test_all_geometry_and_headers_are_compared(self):
        words=[0]*184
        for offset,tag in TAGS.items():words[offset*4:offset*4+4]=tag
        original=packed([0,46]+words)
        native=packed([0,1]+[0]*144)
        inputs=fixture()[:216]
        self.assertEqual(compare(original,native,inputs)['ordered_vertices_exact'],36)
        for index in range(184):
            bad=bytearray(original);bad[(2+index)*4]^=1
            with self.subTest(original_word=index):
                with self.assertRaises(ValueError):compare(bad,native,inputs)
        for index in range(146):
            bad=bytearray(native);bad[index*4]^=1
            with self.subTest(native_word=index):
                with self.assertRaises(ValueError):compare(original,bad,inputs)
        for bad in (b'',original[:-4],original+packed([0])):
            with self.assertRaises(ValueError):compare(bad,native,inputs)
        with self.assertRaises(ValueError):compare(original,native[:-4],inputs)
        with self.assertRaises(ValueError):compare(original,native+packed([0]),inputs)
        with self.assertRaises(ValueError):compare(original,native,fixture()[:432])

    def test_cull_status_and_fixture_contract(self):
        inputs=bytearray(fixture()[:216]);native=packed([0,0])
        original=packed([0,1]+list(TAGS[45]))
        self.assertEqual(compare(original,native,inputs)['culled_prisms'],1)
        for word,value in ((0,1),(1,2),(2,0x7fc00000),(9,64)):
            bad=bytearray(inputs);struct.pack_into('<I',bad,word*4,value)
            with self.assertRaises(ValueError):compare(original,native,bad)
        struct.pack_into('<I',inputs,4,0xffffffff)
        self.assertEqual(compare(packed([0,0]),native,inputs)['culled_prisms'],1)
        with self.assertRaises(ValueError):compare(original,native,inputs)
        for bad in (b'',inputs[:-4],inputs*129):
            with self.assertRaises(ValueError):compare(original,native,bad)
        bad=bytearray(original);bad[-1]^=1
        with self.assertRaises(ValueError):compare(bad,native,fixture()[:216])
