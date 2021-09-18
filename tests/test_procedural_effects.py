import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_procedural_effects import piece,fixture,compare
from verify_shadow_drawing import Reader

def packet(command,fmt,values):
    body=struct.pack('<I',command|(len(values)<<16))
    body+=b''.join(struct.pack(fmt,*v) for v in values)
    body+=bytes(-len(body)%16)
    if len(body)<32:body+=bytes(16)
    return struct.pack('<4I',0x10000000|len(body)//16,0,0,0)+body+bytes(16)

class ProceduralEffectsTests(unittest.TestCase):
    def test_position_and_uv_tags_follow_indices(self):
        parts=[packet(0x79008001,'<3h',[(0,0,0),(64,0,0),(0,64,0)]),
               packet(0x750041aa,'<2H',[(0,0),(32,64),(128,256)]),
               packet(0x6e000000,'<4B',[(255,255,255,255)]),
               packet(0x6e000000,'<4B',[(2,0,0,1),(0,0,0,2),(1,0,0,0)])]
        data=struct.pack('<8I',3,4,5,6,3,7,8,9)+b''.join(struct.pack('<I',len(p))+p for p in parts)
        vertices=piece(Reader(data),True)
        self.assertEqual([v[-2:] for v in vertices],[(6,8),(4,9),(5,7)])
        self.assertEqual(vertices[0][:3],(0.,1.,0.))
        bad=bytearray(data);struct.pack_into('<I',bad,4,256)
        with self.assertRaisesRegex(ValueError,'tag value'):piece(Reader(bad),True)
        with self.assertRaises(ValueError):piece(Reader(data[:-1]),True)

    def test_fixture_and_rng_identity(self):
        self.assertEqual(len(fixture()),15*12)
        original=struct.pack('<5I',0,43,1,100,1)
        native=struct.pack('<5I',0,43,1,101,24)
        with self.assertRaisesRegex(ValueError,'RNG'):compare(original,native)
