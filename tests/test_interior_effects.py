import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_interior_effects import REFERENCES,compare_headers

class ReferenceHeaderTests(unittest.TestCase):
    def fixture(self):
        original=[];native=[]
        for lookup in REFERENCES:
            a=[lookup]+[0]*24;b=a.copy()
            if lookup==57:a[4],b[4]=31,13
            original.extend(a);native.extend(b)
        return struct.pack('<550I',*original),struct.pack('<550I',*native)

    def test_every_other_word_must_be_exact(self):
        a,b=self.fixture()
        report=compare_headers(a,b)
        self.assertEqual(report['words_exact'],549)
        self.assertEqual(report['unimplemented_composite_expansion']['lookup'],57)
        for i in range(550):
            bad=bytearray(b);bad[i*4]^=1
            with self.assertRaises(ValueError):compare_headers(a,bad)

    def test_extent_order_and_known_failure_are_checked(self):
        a,b=self.fixture()
        for x,y in ((a[:-4],b),(a,b+b'\0'*4),(b,b),(a,a)):
            with self.assertRaises(ValueError):compare_headers(x,y)
        bad=bytearray(a);bad[0]^=1
        with self.assertRaises(ValueError):compare_headers(bad,bad)
