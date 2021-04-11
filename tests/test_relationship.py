import hashlib
import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_relationship import cases,compare,CASES_SHA

class RelationshipAuditTests(unittest.TestCase):
    def test_fixture_covers_each_selected_code_and_boundary(self):
        data=cases();self.assertEqual(hashlib.sha256(data).hexdigest(),CASES_SHA)
        rows=list(struct.iter_unpack('<10I',data));self.assertEqual(len(rows),156)
        self.assertEqual({(r[0],r[1+r[0]]) for r in rows},{(i,c) for i in range(3) for c in range(13)})

    def test_auditor_rejects_changed_word_and_missing_case(self):
        original=[];native=[]
        for i in range(156):
            values=(0,1,2,3,4,5)
            n=values+(0x8000,0,1,0,5,4,0x50,5,255,i//52)
            native.extend(n);original.extend((11,)+values+(0x8910,9,0xfffffffe,7,5,255)+n)
        raw=struct.pack('<'+str(len(original))+'I',*original)
        actual=struct.pack('<'+str(len(native))+'I',*native)
        self.assertEqual(compare(raw,actual),2496)
        bad=bytearray(actual);bad[0]^=1
        with self.assertRaises(ValueError):compare(raw,bad)
        with self.assertRaises(ValueError):compare(raw,actual[:-64])
        bad=bytearray(raw);bad[0]^=1
        with self.assertRaises(ValueError):compare(bad,actual)
