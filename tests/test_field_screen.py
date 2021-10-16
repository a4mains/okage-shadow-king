import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_field_screen import compare, fixture, write_retained

def pack(words):
    return struct.pack('<%dI' % len(words), *words)

def synthetic_outputs(inputs):
    """Construct comparison-format examples, not an arithmetic oracle."""
    original, native = [], []
    for index, row in enumerate(struct.iter_unpack('<29I', inputs)):
        mode = row[1]
        words = [index, mode, 0, 10] + [0] * 111
        if mode & 2:
            words[48:60] = row[17:29]
        prim = 0x5d if mode & 1 else 0x4d
        words[63:67] = [3, 0x30004000 | prim << 15 | prim, 0x412 if mode & 1 else 0x41f, 0]
        packed = []
        for vertex in range(3):
            xyzf = [0x12345678 + vertex, 0xfedcba98 + vertex, 0x01234567 + vertex, 0x8000 | vertex << 4]
            words[75 + vertex*12:79 + vertex*12] = xyzf
            packed.extend(xyzf)
            words[103 + vertex*4:107 + vertex*4] = [xyzf[0] & 0xffff, xyzf[1] & 0xffff,
                                                    (xyzf[2] & 0x0ffffff0) >> 4, (xyzf[3] & 0xff0) >> 4]
        original.extend(words)
        native.extend(words[:2] + words[4:63] + packed + words[103:115])
    return pack(original), pack(native)

class FieldScreenTests(unittest.TestCase):
    def test_fixture_scope_winding_and_variety(self):
        inputs = fixture()
        self.assertEqual(inputs, fixture())
        rows = list(struct.iter_unpack('<2I27f', inputs))
        self.assertEqual(len(rows), 64)
        self.assertEqual([sum(row[1] == mode for row in rows) for mode in range(4)], [16] * 4)
        settings = set()
        for case, row in enumerate(rows):
            self.assertEqual(row[0], case)
            self.assertTrue(all(math.isfinite(v) for v in row[2:]))
            s, points = row[2:17], [row[i:i+4] for i in (17, 21, 25)]
            settings.add(s)
            if row[1] < 2:
                for x, y, z, w in points:
                    self.assertEqual(w, 1)
                    self.assertLess(s[9], z)
                    self.assertLess(z, s[10])
                    self.assertLess(abs(x * s[0] * s[1] / z), s[13])
                    self.assertLess(abs(y * s[0] * s[2] / z), s[14])
                xy = [(x/z, y/z) for x, y, z, _ in points]
            else:
                xy = [(x, y) for x, y, _, _ in points]
            (ax, ay), (bx, by), (cx, cy) = xy
            self.assertGreater((bx-ax)*(cy-ay) - (by-ay)*(cx-ax), 0)
        self.assertGreater(len(settings), 8)

    def test_exact_mapping_and_every_native_word(self):
        inputs = fixture()[:116]
        original, native = synthetic_outputs(inputs)
        self.assertEqual(compare(original, native, inputs)['packed_words_exact'], 12)
        words = list(struct.unpack('<85I', native))
        for at in range(85):
            changed = words.copy()
            changed[at] ^= 1
            with self.subTest(word=at), self.assertRaises(ValueError):
                compare(original, pack(changed), inputs)
        inputs = fixture()
        original, native = synthetic_outputs(inputs)
        report = compare(original, native, inputs)
        self.assertEqual(report['mode_counts'], [16] * 4)
        self.assertEqual(report['camera_cache_vertices_exact'], 96)
        self.assertEqual(report['direct_screen_vertices_exact'], 96)

    def test_original_metadata_packet_decode_and_extent(self):
        inputs = fixture()[:116]
        original, native = synthetic_outputs(inputs)
        words = list(struct.unpack('<115I', original))
        for at in (0, 1, 2, 3, 60, 61, 62, 63, 64, 65, 66, 75, 87, 99, 103, 107, 111):
            changed = words.copy()
            changed[at] ^= 1
            with self.subTest(word=at), self.assertRaises(ValueError):
                compare(pack(changed), native, inputs)
        for bad in (b'', original[:-4], original + b'\0\0\0\0'):
            with self.assertRaises(ValueError):
                compare(bad, native, inputs)
        for bad in (b'', native[:-4], native + b'\0\0\0\0'):
            with self.assertRaises(ValueError):
                compare(original, bad, inputs)
        for bad in (b'', inputs[:-4], inputs * 65):
            with self.assertRaises(ValueError):
                compare(original, native, bad)
        for at, value in ((0, 1), (1, 4), (2, 0x7fc00000)):
            changed = bytearray(inputs)
            struct.pack_into('<I', changed, at * 4, value)
            with self.assertRaises(ValueError):
                compare(original, native, changed)

    def test_direct_screen_cache_must_equal_fixture(self):
        inputs = bytearray(fixture()[32*116:33*116])
        struct.pack_into('<I', inputs, 0, 0)
        original, native = synthetic_outputs(inputs)
        compare(original, native, inputs)
        for at in (36, 48):
            changed = bytearray(original)
            struct.pack_into('<I', changed, at * 4, 1)
            with self.assertRaisesRegex(ValueError, 'direct input/cache'):
                compare(changed, native, inputs)

    def test_retained_writes_refuse_changes_and_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'fixture.bin'
            write_retained(path, b'original')
            write_retained(path, b'original')
            with self.assertRaises(ValueError):
                write_retained(path, b'changed')
            self.assertEqual(path.read_bytes(), b'original')
            link = Path(directory) / 'link.bin'
            link.symlink_to(path)
            with self.assertRaises(ValueError):
                write_retained(link, b'original')
