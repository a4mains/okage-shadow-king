import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_shadow_drawing import compare_triangles,compare_uv,Reader,shape_fixture,drawing_fixture

class ShadowDrawingTests(unittest.TestCase):
    def test_sphere_triangle_multiplicity_and_attributes(self):
        triangle=lambda x:[(x,0.,0.,1.,255.,255.,255.,255.,0.,0.,0.,0.),
                           (x,1.,0.,1.,255.,255.,255.,255.,0.,0.,0.,0.),
                           (x,0.,1.,1.,255.,255.,255.,255.,0.,0.,0.,0.)]
        expected=sum((triangle(float(i)) for i in range(224)),[])
        self.assertEqual(compare_triangles(expected,list(reversed(expected)),3),672)
        changed=expected[:];changed[:3]=changed[3:6]
        with self.assertRaises(ValueError):compare_triangles(expected,changed,3)
        changed=expected[:];changed[0]=changed[0][:7]+(254.,)+changed[0][8:]
        with self.assertRaises(ValueError):compare_triangles(expected,changed,3)
        with self.assertRaises(ValueError):compare_triangles(expected,expected[:-3],3)
        edge=sum((triangle(float(i)) for i in range(80)),[])
        with self.assertRaises(ValueError):compare_triangles(edge,list(reversed(edge)),55)

    def test_uv_identity_and_persistent_values(self):
        data=b''.join(struct.pack('<7I',step,i,4,0,1024,0x4200400,0x4200000)
                      for step in range(84) for i in (5,6,7))
        self.assertEqual(compare_uv(data,data),1008)
        for word in (0,1,2,3,7*30+6):
            bad=bytearray(data);bad[word*4]^=1
            with self.assertRaises(ValueError):compare_uv(data,bad)
        bad=bytearray(data);bad[0]^=1
        with self.assertRaisesRegex(ValueError,'identity'):compare_uv(bad,bad)
        with self.assertRaises(ValueError):compare_uv(data[:-4],data[:-4])

    def test_fixture_and_reader_extents(self):
        self.assertEqual(len(shape_fixture()),60)
        self.assertEqual(len(drawing_fixture()),5*288*4)
        r=Reader(b'1234')
        with self.assertRaises(ValueError):r.take(5)
        with self.assertRaises(ValueError):r.take(-1)
        with self.assertRaises(ValueError):r.finish()
