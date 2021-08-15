import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_deferred_effects import audit_order, MANAGER, ORDINARY, DEFERRED, NPC, UPDATE
from verify_torch_drawing import fixture, compare_matrices, packet_vertices

def order_fixture():
    rows=[]
    for frame in range(30):
        def row(pc,slot=None):
            r=[0]*16+[0xffffffff]*84
            r[0:2]=frame,pc
            r[5]=0xffffffff
            if pc==ORDINARY:r[2]=0x1012c4
            if pc==DEFERRED or slot is not None and slot<3:
                r[4]=3;r[16:19]=0,1,2
            if pc==DEFERRED:r[2]=0x1012d8
            if slot is not None:
                r[2]=0x102e54 if slot<3 else 0x1024d8
                r[5]=slot;r[7]=16 if slot<3 else 19 if slot==32 else 91
                r[8]=0x8423 if slot<3 else 0x8020
            rows.append(r)
        row(MANAGER);row(ORDINARY)
        for slot in range(3,33):row(UPDATE,slot)
        row(DEFERRED)
        for slot in range(3):row(UPDATE,slot)
        for _ in range(5):row(NPC)
    return rows

class DeferredEffectsTests(unittest.TestCase):
    def test_queue_order_extent_and_eligibility(self):
        rows=order_fixture()
        pack=lambda rows:b''.join(struct.pack('<100I',*r) for r in rows)
        source=pack(rows)
        self.assertEqual(audit_order(source,'home1b')['effect_updates'],990)
        for row,word,value in [(32,1,NPC),(32,16,2),(33,2,0x1024d8),
                               (34,5,0),(33,8,0x8523),(36,4,1),(41,0,0)]:
            bad=[r[:] for r in rows];bad[row][word]=value
            with self.assertRaises(ValueError):audit_order(pack(bad),'home1b')
        for bad in (source[:-1],source[400:],source+b'\0'):
            with self.assertRaises(ValueError):audit_order(bad,'home1b')

    def test_deferred_geometry_scope(self):
        data=fixture(True)
        self.assertEqual(len(data),5*134*4)
        for i in range(3):
            self.assertEqual(struct.unpack_from('<4I',data,152+i*80+64),(28,0x108,8,4))
        self.assertEqual([struct.unpack_from('<h',data,392+i*48)[0] for i in range(3)],[32,-32,32])
        source=b''.join(struct.pack('<61I',c,*([0]*60)) for c in range(5))
        self.assertEqual(compare_matrices(source,source,True),0)
        for word in range(305):
            bad=bytearray(source);struct.pack_into('<I',bad,word*4,0x3f800000)
            with self.assertRaises(ValueError):compare_matrices(source,bad,True)
        with self.assertRaises(ValueError):compare_matrices(source,source)
        # An ordinary additive TEST register is not the deferred RGB_ONLY one.
        wrong=struct.pack('<10I',0,28,0,0x3f005f,0,1,0x48,0,0x50000,0)
        with self.assertRaises(ValueError):packet_vertices(wrong,True)
