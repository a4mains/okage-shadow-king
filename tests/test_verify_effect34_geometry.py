from pathlib import Path
import struct
import sys
import unittest

TOOLS=Path(__file__).resolve().parents[1]/'tools'
sys.path.insert(0,str(TOOLS))
import verify_effect34_geometry as v

class Effect34GeometryTests(unittest.TestCase):
    def quads(self):
        out=[]
        for step in range(452):
            row=[step]
            for i in range(14):row.extend([i,28 if i<3 else 31]+[0,0,0]*4)
            out.extend(row)
        return struct.pack(f'<{len(out)}I',*out)

    def test_complete_quads_and_reject_missing_or_wrong_ownership(self):
        data=self.quads()
        self.assertEqual(v.compare_quads(data,data)['packed_xyz_uv_words_exact'],75936)
        with self.assertRaisesRegex(ValueError,'Incomplete'):v.quad_rows(data[:-788])
        wrong=bytearray(data);struct.pack_into('<I',wrong,4,1)
        with self.assertRaisesRegex(ValueError,'identity'):v.quad_rows(wrong)
        wrong=bytearray(data);struct.pack_into('<I',wrong,16,0x10000)
        with self.assertRaisesRegex(ValueError,'heap'):v.quad_rows(wrong)
        wrong=bytearray(data);struct.pack_into('<I',wrong,20,17)
        with self.assertRaisesRegex(ValueError,'record0 word5'):v.compare_quads(data,wrong)

    def test_alias_change_only_after_real_update(self):
        out=[]
        for step in range(452):out.extend((step,0,1,2,3,3,3,6,*((3,)*7 if step<2 else (6,)*7)))
        raw=struct.pack(f'<{len(out)}I',*out)
        self.assertIn('ID14',v.check_aliases(raw))
        wrong=bytearray(raw);struct.pack_into('<I',wrong,(15+8)*4,6)
        with self.assertRaisesRegex(ValueError,'sharing'):v.check_aliases(wrong)

    def test_color_exact_and_matrix_finite_bounds(self):
        a=struct.pack('<281I',*([0]*281))
        b=bytearray(a);struct.pack_into('<f',b,4,1e-6)
        with self.assertRaisesRegex(ValueError,'no tolerance'):v.compare_draws(a,b,1)
        struct.pack_into('<I',b,(1+16)*4,1)
        with self.assertRaisesRegex(ValueError,'Exact'):v.compare_draws(a,b,1)
        b=bytearray(a);struct.pack_into('<f',b,4,float('nan'))
        with self.assertRaisesRegex(ValueError,'Exact draw'):v.compare_draws(a,b,1)
        b=bytearray(a);struct.pack_into('<I',b,4*4,1)
        with self.assertRaisesRegex(ValueError,'Exact'):v.compare_draws(a,b,1)
        with self.assertRaisesRegex(ValueError,'coverage'):v.compare_draws(a,a,2)

    def test_storage_aliases_and_mutation_are_required(self):
        quads=bytearray(self.quads());out=[]
        for step in range(452):
            row=[step]
            for i in range(14):
                positions=[0]*8
                if step and i<3:
                    positions[0]=32
                    struct.pack_into('<I',quads,(step*197+3+i*14)*4,32)
                row.extend([200+i*48,100,1000+i*64,2000,100,3000]+positions)
            out.extend(row)
        data=struct.pack(f'<{len(out)}I',*out)
        self.assertIn('mutable template',v.check_storage(data,quads))
        bad=bytearray(data);struct.pack_into('<I',bad,(1+14+2)*4,1000)
        with self.assertRaisesRegex(ValueError,'independently'):v.check_storage(bad,quads)
        bad=bytearray(data);struct.pack_into('<I',bad,(1+14+4)*4,104)
        with self.assertRaisesRegex(ValueError,'relation'):v.check_storage(bad,quads)
        bad=bytearray(data);struct.pack_into('<I',bad,4,0x2000000)
        with self.assertRaisesRegex(ValueError,'outside'):v.check_storage(bad,quads)

    def test_packet_bounds_reject_truncation_and_material(self):
        with self.assertRaisesRegex(ValueError,'extent'):v.packet_cases(b'',1)
        raw=struct.pack('<11I',0,0,28,0,0,0,1,0x44,0,0x50000,0)
        with self.assertRaisesRegex(ValueError,'material'):v.packet_cases(raw,1)
        raw=struct.pack('<12I',0,0,28,0,0,0,1,0x48,0,0x50000,0,0xfffffff0)
        with self.assertRaisesRegex(ValueError,'size'):v.packet_cases(raw,1)

if __name__=='__main__':unittest.main()
