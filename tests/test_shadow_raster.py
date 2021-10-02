import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_shadow_raster import compare,fixture

class ShadowRasterTests(unittest.TestCase):
    def test_pixel_channels_and_extent(self):
        inputs=fixture()[:232]
        pixels=bytes((231,117,33,64))*(640*224)
        exact=compare(pixels,pixels,inputs)[0]
        self.assertEqual(exact['different_bytes'],0)
        self.assertEqual(exact['original_changed_pixels'],0)
        changed=bytearray(pixels)
        for channel in range(4):changed[(10*640+7)*4+channel]^=1
        result=compare(pixels,changed,inputs)[0]
        self.assertEqual(result['different_bytes'],4)
        self.assertEqual(result['different_pixels'],1)
        self.assertEqual([(d['x'],d['y'],d['channel']) for d in result['first_differences']],[(7,10,c) for c in range(4)])
        for bad in (b'',inputs[:-4],inputs*49):
            with self.assertRaises(ValueError):compare(pixels,pixels,bad)
        for bad in (b'',pixels[:-4],pixels+b'\0\0\0\0'):
            with self.assertRaises(ValueError):compare(bad,bad,inputs)
