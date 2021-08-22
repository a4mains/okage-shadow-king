import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from verify_summon_drawing import METADATA, fixture, unpack_records, packet_vertices, compare_matrices

class SummonDrawingTests(unittest.TestCase):
    def test_chunked_positions_and_invalid_vif(self):
        payload = bytearray()
        for first, count in ((0, 53), (53, 7)):
            payload += struct.pack('<I', 0x79008001 | count << 16)
            payload += b''.join(struct.pack('<3h', i, -i, 2*i) for i in range(first, first+count))
            payload += b'\0'*((-len(payload)) % 4)
        payload += b'\0'*((-len(payload)) % 16)
        data = struct.pack('<4I', 0x10000000 | len(payload)//16, 0, 0, 0)+payload+bytes(16)
        chunks = unpack_records(data, 0x79, '<3h')
        self.assertEqual(list(map(len, chunks)), [53, 7])
        self.assertEqual([v for chunk in chunks for v in chunk], [(i, -i, 2*i) for i in range(60)])
        for bad in (data[:-1], data[:-16], data+bytes(16)):
            with self.assertRaises(ValueError):
                unpack_records(bad, 0x79, '<3h')
        for command in (0xff000000, 0x79ff8001, 0x79018002):
            bad = bytearray(data); struct.pack_into('<I', bad, 16, command)
            with self.assertRaises(ValueError):
                unpack_records(bad, 0x79, '<3h')

    def test_fixtures_and_transform_changes(self):
        for template, metadata in METADATA.items():
            data = fixture(template)
            stride = (38+len(metadata)*20+(30 if template == 33 else 0))*4
            self.assertEqual(len(data), 5*stride)
            for case in range(5):
                self.assertEqual(struct.unpack_from('<I', data, case*stride)[0], case)
                for i, expected in enumerate(metadata):
                    self.assertEqual(struct.unpack_from('<4I', data, case*stride+216+i*80), expected)
            width = 1+20*len(metadata)
            original = b''.join(struct.pack(f'<{width}I', case, *([0]*(width-1))) for case in range(5))
            self.assertEqual(compare_matrices(original, original, template), 5*len(metadata))
            for word in (0, 1, width-1, 5*width-1):
                bad = bytearray(original); struct.pack_into('<I', bad, word*4, 0x3f800000)
                with self.assertRaises(ValueError):
                    compare_matrices(original, bad, template)
            with self.assertRaises(ValueError):
                compare_matrices(original, original[:-4], template)

    def test_wrong_materials_and_piece_count(self):
        wrong = struct.pack('<11I', 0, 32, 0, 0xffffffff, 0, 1, 0x48, 0, 0x50000, 0, 1)
        with self.assertRaisesRegex(ValueError, 'material'):
            packet_vertices(wrong, 115)
        wrong = struct.pack('<11I', 0, 40, 0, 0xffffffff, 0, 1, 0x48, 0, 0x50000, 0, 0)
        with self.assertRaisesRegex(ValueError, 'pieces'):
            packet_vertices(wrong, 33)
