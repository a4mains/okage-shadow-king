import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_message_opening import initialization_projection,compare_controller

class MessageOpeningAuditTests(unittest.TestCase):
    def test_initializer_identity_and_extent(self):
        stride=0xef0+2*0x6ec0;data=bytearray(stride*3)
        for phase in range(3):
            values={0x8c:10001,0x16c:10001,0xe40:2,0xe4c:2 if phase==2 else 0,
                    0x90:0x50,0x170:0x50,0xed8:0xffffffff}
            for offset,value in values.items():struct.pack_into('<I',data,phase*stride+offset,value)
        self.assertEqual(len(initialization_projection(data)),360)
        for offset in [0x8c,0x16c,0xe40,0xe4c,0x88,0x90,0xed8]:
            changed=bytearray(data);changed[offset]^=1
            with self.assertRaises(ValueError):initialization_projection(changed)
        with self.assertRaises(ValueError):initialization_projection(data[:-4])

    def test_live_comparison_rejects_one_word_and_frame_changes(self):
        reference=[];native=[]
        for frame in range(1,5851):native.append([frame,10001]+[0]*10)
        for frame in range(131,251):
            for pc in [0x148dc8,0x11ff38]:reference.append([frame,pc]+[0]*62)
        source=b''.join(struct.pack('<64I',*r) for r in reference)
        live=b''.join(struct.pack('<12I',*r) for r in native)
        self.assertEqual(compare_controller(source,live),1200)
        for offset in [5729*48,5729*48+4,5729*48+8,5848*48+44]:
            changed=bytearray(live);changed[offset]^=1
            with self.assertRaises(ValueError):compare_controller(source,changed)
        with self.assertRaises(ValueError):compare_controller(source[:-4],live)

if __name__=='__main__':unittest.main()
