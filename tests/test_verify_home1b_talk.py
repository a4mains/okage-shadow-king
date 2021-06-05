from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
from verify_home1b_talk import compare_hero

class Home1bRootTests(unittest.TestCase):
    def setUp(self):
        self.root=struct.unpack('<8I',struct.pack('<8f',0.,.2,0.,0.,1.,2.,3.,1.))

    def test_existing_position_bound_is_explicit(self):
        changed=list(self.root);changed[4]+=1;changed=tuple(changed)
        with self.assertRaises(ValueError):compare_hero(changed,self.root)
        self.assertGreater(compare_hero(changed,self.root,True),0.)

    def test_rotation_and_w_stay_exact(self):
        for index in (0,1,2,3,7):
            changed=list(self.root);changed[index]^=1
            with self.subTest(index=index),self.assertRaises(ValueError):
                compare_hero(tuple(changed),self.root,True)

    def test_nonfinite_and_excessive_error_are_rejected(self):
        for value in (float('nan'),float('inf'),1.0001):
            changed=list(self.root);changed[4]=struct.unpack('<I',struct.pack('<f',value))[0]
            with self.subTest(value=value),self.assertRaises(ValueError):
                compare_hero(tuple(changed),self.root,True)

    def test_truncation_is_rejected(self):
        with self.assertRaises(ValueError):compare_hero(self.root[:-1],self.root,True)
