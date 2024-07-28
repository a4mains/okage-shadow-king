from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from verify_loop_stream import audit_source

class LoopSourceValidation(unittest.TestCase):
    def setUp(self):
        self.payload=bytes(range(64))
        self.data=self.payload+self.payload[16:]*3
        self.rows=struct.pack('<4I', 2, 100, 64, 16)
        end=0
        for count, pointer in [(64,1000),(48,1016),(48,1016),(48,1016)]:
            end+=count
            self.rows+=struct.pack('<4I',end,pointer,count,0)

    def audit(self, data=None, rows=None):
        audit_source(self.payload,100,16,self.data if data is None else data,
                     self.rows if rows is None else rows)

    def test_all_source_passes(self):
        self.audit()

    def test_corruption_at_each_loop_boundary(self):
        for at in [0,63,64,111,112,159,160,207]:
            bad=bytearray(self.data);bad[at]^=1
            with self.subTest(at=at), self.assertRaises(ValueError):self.audit(data=bad)

    def test_missing_observation_and_false_end(self):
        for rows in [self.rows[:-16],self.rows[:-1],self.rows[:16]]:
            with self.assertRaises(ValueError):self.audit(rows=rows)
        for at in [0,4,8,12,16,20,24,28,36]:
            bad=bytearray(self.rows);struct.pack_into('<I',bad,at,0xffffffff)
            with self.subTest(at=at),self.assertRaises(ValueError):self.audit(rows=bad)
