import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_shadow_visibility import compare,fixtures

class ShadowVisibilityTests(unittest.TestCase):
    def test_output_order_and_integrity(self):
        cases=[dict(id=1,culling=0,blocks=[[],[],[]])]
        data=struct.pack('<4I',1,3,2,0x412e0000)+struct.pack('<4I',0,0x41220000,1,0x41220000)
        self.assertEqual(compare(data,data,cases)['selected_blocks_exact'],3)
        for i in range(len(data)):
            bad=bytearray(data);bad[i]^=1
            with self.assertRaises(ValueError):compare(data,bad,cases)
        for bad in [b'',data[:-4],data+bytes(4),struct.pack('<2I',1,0)]:
            with self.assertRaises(ValueError):compare(bad,bad,cases)
        for offset,value in [(8,3),(24,0),(20,0x41300000),(12,0x7fc00000)]:
            bad=bytearray(data);struct.pack_into('<I',bad,offset,value)
            with self.assertRaises(ValueError):compare(bad,bad,cases)

    def test_fixtures_are_deterministic_and_include_rejection(self):
        a,cases=fixtures();b,other=fixtures()
        self.assertEqual(a,b);self.assertEqual(cases,other)
        self.assertGreater(len(cases),80)
        self.assertTrue(any(not c['culling'] for c in cases))
        self.assertEqual(compare(struct.pack('<2I',1,0),struct.pack('<2I',1,0),
                                 [dict(id=1,culling=1,blocks=[[]])])['empty_cases_exact'],1)
