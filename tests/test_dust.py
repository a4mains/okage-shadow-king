import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_dust import compare_state
from verify_dust_scene import map_rows

class DustAuditTests(unittest.TestCase):
    def test_scene_trace_extent_and_identity(self):
        header=struct.pack('<22I',1,17,0,1,30,9,*([0]*16))
        job=header+bytes(9*80)
        source=struct.pack('<2I',1,1)+job
        self.assertEqual(map_rows(source)[0][1][0][1],17)
        for n in [1,7,20,len(source)-1]:
            with self.assertRaises(ValueError):map_rows(source[:n])
        with self.assertRaises(ValueError):map_rows(struct.pack('<2I',1,2)+job+job)
        with self.assertRaises(ValueError):map_rows(struct.pack('<2I',2,0))
        with self.assertRaises(ValueError):map_rows(struct.pack('<2I',1,85))

    def test_exact_state_and_lifetime(self):
        rows=[]
        for step in range(33):
            row=[0]*425
            row[:7]=[step,0,17,0 if step<2 else 0x120 if step==32 else 0x20,
                     9,30,1 if step<2 else min(step,31)]
            rows.append(struct.pack('<425I',*row))
        original=b''.join(rows)
        self.assertEqual(compare_state(original,original),14025)
        for word in [3,6,7,8,29,32*425+3,3*425+29+25]:
            bad=bytearray(original);bad[word*4]^=1
            with self.assertRaises(ValueError):compare_state(original,bad)
        for word in [0,2,3,4,5,6,32*425+3]:
            bad=bytearray(original);bad[word*4]^=1
            with self.assertRaises(ValueError):compare_state(bad,bad)
        with self.assertRaises(ValueError):compare_state(original[:-4],original[:-4])

if __name__=='__main__':unittest.main()
