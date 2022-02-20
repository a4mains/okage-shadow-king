import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import verify_battle_camera as camera

def fixture(ticks=2):
    row = bytearray(camera.ROW_SIZE)
    struct.pack_into('<f3I', row, 0, 1., ticks, 1, 0)
    return row

class BattleCameraVerifierTests(unittest.TestCase):
    def test_exact_extent_and_explicit_scope(self):
        inputs = fixture(0) + fixture(2)
        output = bytes(range(224)) * 4
        result = camera.compare(inputs, output, output)
        self.assertTrue(result['exact'])
        self.assertEqual((result['rows'], result['snapshots'], result['compared_bytes']), (2, 4, 896))
        self.assertIsNone(result['first_difference'])
        self.assertFalse(result['independent_algorithm_check'])

    def test_input_extent_and_header_bounds(self):
        for data in (b'', fixture()[:-1], fixture() + b'\0', fixture() * 17):
            with self.subTest(length=len(data)), self.assertRaisesRegex(ValueError, 'input extent'):
                camera.rows(data)
        for offset, kind, value in ((0, 'f', 0.), (0, 'f', -1.), (0, 'f', float('nan')),
                                    (0, 'f', float('inf')), (4, 'I', 2001),
                                    (8, 'I', 0), (8, 'I', 2), (8, 'I', 8), (12, 'I', 1)):
            data = fixture()
            struct.pack_into('<' + kind, data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaisesRegex(ValueError, 'header bounds'):
                camera.rows(data)
        self.assertEqual(len(camera.rows(fixture(2000) * 16)), 16)

    def test_present_preset_bounds_and_absent_slots(self):
        for offset, kind, value in ((0, 'I', 2), (48, 'I', 7), (44, 'f', 1.),
                                    (4, 'f', -1.), (52, 'f', float('inf')),
                                    (100, 'f', -1.), (112, 'f', float('nan')),
                                    (16, 'f', float('nan')), (92, 'f', 1000001.),
                                    (0, 'I', 1), (48, 'I', 1)):
            data = fixture()
            struct.pack_into('<' + kind, data, 16 + offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                camera.rows(data)
        data = fixture()
        data[160:] = b'\xff' * 288
        struct.pack_into('<I', data, 16 + 44, 0x80000000)
        self.assertEqual(camera.rows(data)[0]['presence_mask'], 1)
        struct.pack_into('<I', data, 8, 3)
        with self.assertRaisesRegex(ValueError, 'preset 1 mode'):
            camera.rows(data)

    def test_rejects_truncated_and_extra_outputs_on_either_side(self):
        inputs, output = fixture(), bytes(224 * 3)
        for name in ('original', 'native'):
            for invalid in (b'', output[:-1], output + bytes(4)):
                pair = {'original': output, 'native': output}
                pair[name] = invalid
                with self.subTest(name=name, length=len(invalid)), self.assertRaisesRegex(ValueError, name + ' output extent'):
                    camera.compare(inputs, **pair)

    def test_first_difference_integer_float_signed_zero_and_phase(self):
        inputs = fixture(0) + fixture(2)
        original = bytes(224 * 4)
        for snapshot, word, bits, row, tick, location in (
                (0, 0, 1, 0, 0, 'body+0x10'),
                (1, 4, 0x3f800000, 1, 0, 'body+0x20'),
                (2, 4, 0x80000000, 1, 1, 'body+0x20'),
                (3, 55, 3, 1, 2, 'sequence_phase')):
            native = bytearray(original)
            struct.pack_into('<I', native, snapshot * 224 + word * 4, bits)
            if snapshot * 224 + word * 4 + 4 < len(native):
                native[-1] = 255
            with self.subTest(snapshot=snapshot, word=word):
                result = camera.compare(inputs, original, native)
                self.assertFalse(result['exact'])
                diff = result['first_difference']
                self.assertEqual((diff['row'], diff['tick'], diff['location']), (row, tick, location))
                self.assertEqual(diff['initial_snapshot'], tick == 0)
                self.assertEqual(diff['word'], word)
                self.assertEqual(diff['original_word'], '0x00000000')
                self.assertEqual(diff['native_word'], f'0x{bits:08x}')
                self.assertEqual(diff['snapshot'], snapshot)

    def test_generator_requires_owned_elf_hash_before_parsing(self):
        for data in (b'', b'\x7fELF' + bytes(4096)):
            with self.subTest(length=len(data)), self.assertRaisesRegex(ValueError, 'pinned USA ELF hash'):
                camera.fixtures(data)

    def test_fresh_output_refuses_overwrite_and_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'sub' / 'output.bin'
            camera.write_fresh(path, b'first')
            with self.assertRaises(FileExistsError):
                camera.write_fresh(path, b'replacement')
            self.assertEqual(path.read_bytes(), b'first')
            link = Path(directory) / 'link'
            link.symlink_to(path)
            with self.assertRaises(FileExistsError):
                camera.write_fresh(link, b'replacement')
            self.assertEqual(path.read_bytes(), b'first')

if __name__ == '__main__':
    unittest.main()
