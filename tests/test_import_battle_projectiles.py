import pathlib
import struct
import sys
import tempfile
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
import import_battle_projectiles as m

class Image:
    def __init__(self):
        table = bytearray()
        for i in range(m.COUNT):
            table += struct.pack('<hh3f', 1+(i % 2), 30, float(i), -2.5, 0.0)
        self.regions = {m.BASE: bytes(table)}
        for start, end, stride, _ in m.REGIONS:
            region = bytearray(end-start)
            self.regions[start] = region
        paired = self.regions[m.REGIONS[0][0]]
        for i in range(m.COUNT):
            struct.pack_into('<I', paired, i*3*0x88+4, m.BASE+i*m.STRIDE)
    def read(self, start, size):
        value = bytes(self.regions[start])
        if size != len(value):
            raise ValueError('fixture read extent')
        return value

class ProjectileTests(unittest.TestCase):
    def test_table_records_and_references(self):
        table, records = m.extract(Image())
        self.assertEqual(len(table), m.COUNT*m.STRIDE)
        self.assertEqual([r['type'] for r in records[:2]], [1, 2])
        self.assertEqual(records[3]['offset'], [3.0, -2.5, 0.0])
        self.assertEqual(records[5]['references'], [{'region': 'paired', 'index': 15}])
    def test_rejects_foreign_misaligned_and_unreferenced(self):
        for pointer in (m.BASE-16, m.BASE+m.COUNT*m.STRIDE, m.BASE+4):
            image = Image()
            struct.pack_into('<I', image.regions[m.REGIONS[1][0]], 4, pointer)
            with self.assertRaisesRegex(ValueError, 'outside table'):
                m.extract(image)
        image = Image()
        struct.pack_into('<I', image.regions[m.REGIONS[0][0]], 4, 0)
        with self.assertRaisesRegex(ValueError, 'unreferenced'):
            m.extract(image)
    def test_rejects_invalid_record_values(self):
        image = Image()
        table = bytearray(image.regions[m.BASE])
        struct.pack_into('<h', table, 2, 0)
        image.regions[m.BASE] = bytes(table)
        with self.assertRaisesRegex(ValueError, 'duration'):
            m.extract(image)
        image = Image()
        table = bytearray(image.regions[m.BASE])
        struct.pack_into('<f', table, 8, float('nan'))
        image.regions[m.BASE] = bytes(table)
        with self.assertRaisesRegex(ValueError, 'Nonfinite'):
            m.extract(image)
    def test_source_pinned_and_fresh_outputs(self):
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            m.import_data(b'not the owned elf')
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp)/'battle-projectiles.bin'
            out.write_bytes(b'')
            with self.assertRaisesRegex(ValueError, 'existing'):
                m.main(['--elf', str(out), '--output', str(out)])

if __name__ == '__main__':
    unittest.main()
