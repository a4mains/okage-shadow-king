import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_interior_scene import submission_rows, compare_order, compare_placement_headers
from test_deferred_effects import order_fixture

class InteriorSceneTests(unittest.TestCase):
    def test_submission_order_lifecycle_and_extent(self):
        def pack(jobs):
            return struct.pack('<2I',1,len(jobs))+b''.join(struct.pack('<4I',*r) for r in jobs)
        good=[(2,91,0x8020,99),(0,16,0x8423,1)]
        self.assertEqual(submission_rows(pack(good)),[good])
        for bad in [good[::-1],good+good[:1],[(84,91,0x8020,1)],
                    [(0,91,0x8120,1)],[(0,91,0xc020,1)]]:
            with self.assertRaises(ValueError):submission_rows(pack(bad))
        for bad in [pack(good)[:-1],pack(good)+b'\0',struct.pack('<2I',2,0)]:
            with self.assertRaises(ValueError):submission_rows(bad)

    def test_order_identity_and_unresolved_phase_are_distinct(self):
        rows=order_fixture();original=b''.join(struct.pack('<100I',*r) for r in rows)
        native=[[(r[5],r[7],r[8]|0x20,1) for r in rows if r[0]==f and r[1]==0x1131d0] for f in range(30)]
        self.assertEqual(compare_order(native,original,1,'home1b')['unresolved_map_phase_mismatches'],{})
        native[0][0]=(*native[0][0][:3],100)
        self.assertEqual(compare_order(native,original,1,'home1b')['unresolved_map_phase_mismatches'],{'91':1})
        native[0][0]=(4,*native[0][0][1:])
        with self.assertRaises(ValueError):compare_order(native,original,1,'home1b')

    def test_placement_phase_exclusion_does_not_hide_other_changes(self):
        ram=bytearray(32*1024*1024);table,job=0x100,0x200
        struct.pack_into('<I',ram,0x1f849c,table);struct.pack_into('<I',ram,table,job)
        header=(0,91,0x8020,123,9900,4)+tuple(range(16))
        for offset,value in [(0,91),(4,0x8020),(8,4),(0x60,9900),(0x64,123)]:
            struct.pack_into('<I',ram,job+offset,value)
        struct.pack_into('<16I',ram,job+0x10,*range(16))
        self.assertEqual(compare_placement_headers({0:header},ram),1)
        for word in range(22):
            bad=list(header);bad[word]+=1
            if word==3:self.assertEqual(compare_placement_headers({0:tuple(bad)},ram),1)
            else:
                with self.assertRaises(ValueError):compare_placement_headers({0:tuple(bad)},ram)
