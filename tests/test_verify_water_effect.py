import struct
import sys
import unittest
from pathlib import Path
ROOT = next(p for p in Path(__file__).resolve().parents if (p/'tools/verify_map_effect.py').is_file())
sys.path.insert(0, str(ROOT/'tools'))
import verify_water_effect as water

def fixture(steps=3):
    geometry,uv,native=bytearray(),bytearray(),bytearray()
    for step in range(steps):
        native+=struct.pack('<5I',step,32,32,2 if step<2 else 3,0x40400000 if step<2 else 0x41200000)
        points=[]
        for i in range(1024):
            x,y=i%32,i//32
            point=(((-32+x*2)&65535)|(((i%17-8)&65535)<<16),(-32+y*2)&65535,(x*64)|(y*65<<16))
            points.append(point);native+=struct.pack('<3I',*point)
        slot=0
        for piece,count in enumerate(water.COUNTS):
            geometry+=struct.pack('<4I',step,0,piece,count)
            uv+=struct.pack('<4I',step,0,piece,count)
            for _ in range(count):
                xy,z,st=points[slot%1024];slot+=1
                geometry+=struct.pack('<3I',xy,z|(0xcafe<<16),0)
                uv+=struct.pack('<2I',st,0)
    return geometry,uv,native

class WaterVerifierTests(unittest.TestCase):
    def setUp(self):self.g,self.u,self.n=fixture()
    def check(self,g=None,u=None,n=None):
        return water.compare_water(self.g if g is None else g,self.u if u is None else u,self.n if n is None else n,steps=3)
    def test_exact_fields_and_only_adjacent_storage_excluded(self):
        r=self.check();self.assertTrue(r['captured_fields_exact'])
        self.assertEqual(r['original_packed_points_compared'],4602)
        self.assertEqual(r['excluded_original_bits']['count'],73632)
        g=bytearray(self.g);g[22]^=255;self.assertTrue(self.check(g=g)['captured_fields_exact'])
    def test_extents_and_original_record_identity(self):
        for what,data in [('g',self.g),('u',self.u),('n',self.n)]:
            for changed in [data[:-1],data+bytes(4)]:
                with self.subTest(what=what,size=len(changed)),self.assertRaises(ValueError):self.check(**{what:changed})
        for name,source,lanes in [('g',self.g,3),('u',self.u,2)]:
            second=16+94*lanes*4
            for offset,value in [(0,1),(4,1),(8,1),(12,93),(second+8,0)]:
                changed=bytearray(source);struct.pack_into('<I',changed,offset,value)
                with self.subTest(name=name,offset=offset),self.assertRaises(ValueError):self.check(**{name:changed})
    def test_native_identity_and_source_assertions_exact(self):
        for offset,value in [(0,1),(4,31),(8,31),(12,3),(16,0x40400001),
                             (water.WATER_WORDS*4,0),(2*water.WATER_WORDS*4+12,2),(2*water.WATER_WORDS*4+16,0x40400000)]:
            n=bytearray(self.n);struct.pack_into('<I',n,offset,value)
            with self.subTest(offset=offset),self.assertRaises(water.WaterMismatch):self.check(n=n)
    def test_coordinates_uv_and_tags_never_relaxed(self):
        for name,source,offset in [('g',self.g,16),('g',self.g,18),('g',self.g,20),('g',self.g,24),
                                   ('u',self.u,16),('u',self.u,20),('n',self.n,20),('n',self.n,22),('n',self.n,24),('n',self.n,26),('n',self.n,28)]:
            changed=bytearray(source);changed[offset]^=1
            with self.subTest(name=name,offset=offset),self.assertRaises(ValueError):self.check(**{name:changed})
    def test_duplicate_mismatch_identifies_original_point_and_grid(self):
        slot=1024;at=0
        for piece,count in enumerate(water.COUNTS):
            if slot<count:break
            slot-=count;at+=16+count*12
        g=bytearray(self.g);offset=at+16+slot*12+2;g[offset]^=1
        with self.assertRaises(water.WaterMismatch) as caught:self.check(g=g)
        self.assertEqual(caught.exception.detail['grid'],[0,0])
        self.assertEqual(caught.exception.detail['piece'],piece)
        self.assertEqual(caught.exception.detail['vertex'],slot)
        self.assertEqual(caught.exception.detail['channel'],'packedXY')
    def test_native_grid_order_is_not_reassigned_by_position(self):
        n=bytearray(self.n);n[20:32],n[32:44]=n[32:44],n[20:32]
        with self.assertRaises(water.WaterMismatch):self.check(n=n)
    def test_state106_cannot_use_float_tolerance(self):
        words=[]
        for step in range(3):words.extend([step,0,106,0,1]+[0]*68)
        reference=struct.pack('<219I',*words);water.compare(reference,reference,106,1,1)
        words[2*73+29+24]=1
        with self.assertRaises(ValueError):water.compare(reference,struct.pack('<219I',*words),106,1,1)

if __name__=='__main__':unittest.main()
