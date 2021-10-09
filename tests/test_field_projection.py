import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_field_projection import compare,reference_frames

class FieldProjectionTests(unittest.TestCase):
    def fixture(self):
        pcs=[0x1b9be0,0x1b9288,0x183f18,0x183df0]+[0x185f20,0x1b8dc8]*7+[
            0x183f18,0x183df0,0x102218,0x183f18,0x183df0,0x102cf8,0x183df0]
        rows=[]
        for pc in pcs:
            r=[0]*300;r[1]=pc;r[16:31]=range(15);r[131]=0x40400000
            r[132:]=[0xffffffff]*168;rows.append(r)
        rows[22][2]=0x102350;rows[22][7:9]=[50,51]
        rows[24][2]=0x102dc8;rows[24][7:9]=[60,61]
        original=b''.join(struct.pack('<300I',*r) for r in rows)
        native=[1,78,1,0x40400000]+list(range(15))*6
        native[4+3*15+10:4+3*15+12]=[50,51]
        for i in (4,5):native[4+i*15+10:4+i*15+12]=[60,61]
        return original,struct.pack('<94I',*native)

    def test_entry_setters_and_all_stages(self):
        a,b=self.fixture();r=compare(a,b,1,78)
        self.assertEqual(r['compared_words'],91);self.assertEqual(r['differences'],[])
        altered=bytearray(b);struct.pack_into('<I',altered,(4+2*15+6)*4,900)
        r=compare(a,altered,1,78)
        self.assertEqual(len(r['differences']),1)
        self.assertEqual(r['differences'][0]['stage'],'field_end')
        self.assertEqual(r['differences'][0]['field'],'clip_near')

    def test_rejects_missing_or_misaligned_evidence(self):
        a,b=self.fixture()
        for original,native,start,scene in ((a[:-1],b,1,78),(a,b[:-4],1,78),(a,b,2,78),(a,b,1,79),(a,b,0,78)):
            with self.assertRaises(ValueError):compare(original,native,start,scene)
        a=bytearray(a);struct.pack_into('<I',a,4,0xdeadbeef)
        with self.assertRaises(ValueError):reference_frames(a)

    def test_rejects_uncovered_near_changes(self):
        a,_=self.fixture();a=bytearray(a)
        struct.pack_into('<II',a,132*4,0x8020,0x40800000)
        with self.assertRaises(ValueError):reference_frames(a)
