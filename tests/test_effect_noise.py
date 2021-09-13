import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_effect_noise import fixture,compare,triangles
from inspect_effect_template_load import inspect

class EffectNoiseTests(unittest.TestCase):
    def test_fixture_and_identity_guards(self):
        self.assertEqual(len(fixture()),24)
        header=struct.pack('<4I',0,1,123,384)
        grid=struct.pack('<384I',*([0]*384))
        for index in range(4):
            altered=bytearray(header);altered[index*4]^=1
            with self.assertRaises(ValueError):compare(header+grid,altered+grid)
        bad=struct.pack('<I',7)+grid[4:]
        with self.assertRaisesRegex(ValueError,'grid'):compare(header+bad,header+bad)
        with self.assertRaisesRegex(ValueError,'extent'):compare(header+grid[:-1],header+grid)
        # Both streams agree on the wrong consumer RNG: still reject it.
        invalid=header+grid+struct.pack('<3I',10,124,2)
        with self.assertRaisesRegex(ValueError,'RNG'):compare(invalid,invalid)

    def test_triangle_comparison_retains_multiplicity(self):
        vertices=[(0,),(1,),(2,),(2,),(3,),(4,)]
        self.assertEqual(triangles(vertices),triangles(list(reversed(vertices))))
        self.assertNotEqual(triangles(vertices),triangles(vertices[:3]*2))

    def test_loader_observation_guards(self):
        state=1;grid=[];trace=[]
        for _ in range(384):
            trace.extend((0x10e0e8,state));state=(state*0x41c64e6d+0x3039)&0xffffffff
            grid.append((state&0x7fffffff)%7)
        words=[0x504d5445,1,state,state,0,384,*grid,*trace,*([0]*132)]
        words.extend(x for i in range(128) for x in (i,0xffffffff,0,0))
        data=struct.pack('<%dI'%len(words),*words)
        self.assertEqual(inspect(data,1,0)['rng_calls'],384)
        for index in (0,2,3,6,390,391,390+768,390+768+132):
            altered=words[:];altered[index]^=0x80000000
            with self.assertRaises(ValueError):inspect(struct.pack('<%dI'%len(altered),*altered),1,0)
        with self.assertRaises(ValueError):inspect(data+b'1234',1,0)
