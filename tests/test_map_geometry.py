import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from verify_map_geometry import compare_matrices, compare_vertices

class MapGeometryAuditTests(unittest.TestCase):
    def test_matrix_scope(self):
        source = b''.join(struct.pack('<161I', i, *([0]*160)) for i in range(5))
        self.assertEqual(compare_matrices(source, source), 0)
        for offset in [0, 17*4, 161*4]:
            bad = bytearray(source);struct.pack_into('<f', bad, offset, .5)
            with self.assertRaises(ValueError):compare_matrices(source, bad)
        for value in [.001, float('nan'), float('inf')]:
            bad = bytearray(source);struct.pack_into('<f', bad, 4, value)
            with self.assertRaises(ValueError):compare_matrices(source, bad)
        with self.assertRaises(ValueError):compare_matrices(source, source[:-4])

    def test_packets_and_vertices(self):
        original = bytearray();native = bytearray()
        for i in range(8):
            shape = 24 if i < 3 else 29
            counts = (32,34,2,34) if i < 3 else (4,4,1,4)
            original.extend(struct.pack('<10I', i,shape,0,0,0,1,0x48,0,0x50000,0))
            for k,(n,offset,stride,command) in enumerate(zip(counts,[48,48,40,48],[6,4,4,4],
                    [0x79008001,0x750041aa,0x6e004149,0x6e00c001])):
                b = bytearray(max(64,(offset+n*stride+15)//16*16))
                if k < 2:struct.pack_into('<I', b, 28, n)
                if k == 2:struct.pack_into('<I', b, 20, n)
                struct.pack_into('<I', b, offset-4, command | n << 16)
                if k == 2:b[offset:offset+n*stride] = bytes([255])*(n*stride)
                original.extend(struct.pack('<I', len(b)));original.extend(b)
            count = (counts[3]-2)*3
            native.extend(struct.pack('<3I', i,count,20 if i < 3 else 13))
            native.extend(struct.pack('<12f', 0,0,0,1,255,255,255,255,0,0,0,0)*count)
        self.assertEqual(compare_vertices(original,native),318)
        for offset in [0,4,8,12,28,40]:
            bad = bytearray(native);bad[offset] ^= 1
            with self.assertRaises(ValueError):compare_vertices(original,bad)
        for offset in [0,4,16,20,24,32,40]:
            bad = bytearray(original);bad[offset] ^= 1
            with self.assertRaises((ValueError,struct.error)):compare_vertices(bad,native)
        with self.assertRaises(ValueError):compare_vertices(original,native[:-4])
        with self.assertRaises(ValueError):compare_vertices(original[:-4],native)

if __name__ == '__main__':unittest.main()
