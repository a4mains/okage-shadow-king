import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_torch_drawing import fixture,compare_matrices,packet_vertices,compare_vertices

class TorchDrawingTests(unittest.TestCase):
    def test_matrix_identity_extent_and_exact_channels(self):
        rows=[]
        for case in range(5):rows.extend([case]+[0]*80)
        source=struct.pack('<405I',*rows)
        self.assertEqual(compare_matrices(source,source),0)
        for word in range(405):
            bad=bytearray(source)
            # Large finite error must fail for every matrix/color/identity word.
            struct.pack_into('<I',bad,word*4,0x3f800000)
            with self.assertRaises(ValueError):compare_matrices(source,bad)
        for bad in (source[:-4],source+b'\0\0\0\0'):
            with self.assertRaises(ValueError):compare_matrices(source,bad)
        for word in (1,4,17):
            bad=bytearray(source);struct.pack_into('<I',bad,word*4,0x7fc00000)
            with self.assertRaises(ValueError):compare_matrices(source,bad)

    def test_fixture_is_bounded_and_has_both_flip_signs(self):
        data=fixture()
        self.assertEqual(len(data),5*154*4)
        self.assertEqual([struct.unpack_from('<I',data,c*616)[0] for c in range(5)],list(range(5)))
        self.assertEqual([struct.unpack_from('<h',data,472+i*48)[0] for i in range(3)],[32,-32,32])

    def test_packet_extents_and_material_fail_closed(self):
        for data in (b'',bytes(39),bytes(40),struct.pack('<10I',0,28,0,0,0,1,0x44,0,0x50000,0)):
            with self.assertRaises(ValueError):packet_vertices(data)
        header=struct.pack('<10I',0,28,0,0x3f005f,0,1,0x48,0,0x50000,0)
        for size in (0,63,65,8193,0xffffffff):
            with self.assertRaises(ValueError):packet_vertices(header+struct.pack('<I',size))
        with self.assertRaises(ValueError):compare_vertices(b'',b'')
