import hashlib
import mmap
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import verify_battle_capture_raster as raster

def header(mode=2, rgba=0xFFFFFFFF, progress=0, field=0, reserved=(0, 0, 0, 0)):
    return struct.pack('<8I', mode, rgba, progress, field, *reserved)

class BattleCaptureRasterTests(unittest.TestCase):
    def test_layout_cases_and_alpha_coverage(self):
        cases = raster.fixture_cases()
        self.assertLessEqual(len(cases), raster.MAX_CASES)
        self.assertEqual(raster.INPUT_ROW_BYTES, 1_966_112)
        self.assertLessEqual(len(cases) * raster.INPUT_ROW_BYTES, 32 * 1024 * 1024)
        self.assertEqual({c['rgba'] & 255 for c in cases[:4]}, {0, 64, 128, 255})
        self.assertEqual({(c['mode'], c['progress'], c['field_nonzero']) for c in cases[:2]},
                         {(2, 0, 0), (2, 0, 1)})
        self.assertEqual(len({c['rgba'] >> 8 for c in cases}), len(cases))

    def test_source_roles_and_padding_rows_have_distinct_texels(self):
        for pattern in ('ramp', 'checker', 'impulse', 'edges'):
            with self.subTest(pattern=pattern):
                roles = [raster._pixel(pattern, role, 17, 31, 0) for role in range(3)]
                self.assertEqual(len(set(roles)), 3)
                self.assertNotEqual(raster._pixel(pattern, 0, 17, 223, 0),
                                    raster._pixel(pattern, 0, 17, 224, 0))

    def test_input_extent_flags_and_reserved_words(self):
        for data in (b'', header()[:-1], header() + b'\0', bytes(raster.INPUT_ROW_BYTES - 1),
                     bytes(raster.INPUT_ROW_BYTES * (raster.MAX_CASES + 1))):
            with self.subTest(length=len(data)), self.assertRaisesRegex(ValueError, 'input extent'):
                raster.rows(data)
        for field, reserved in ((2, (0, 0, 0, 0)), (0, (0, 1, 0, 0))):
            bad = header(field=field, reserved=reserved) + bytes(raster.INPUT_ROW_BYTES - raster.HEADER_BYTES)
            with self.subTest(field=field, reserved=reserved), self.assertRaisesRegex(ValueError, 'flags/reserved'):
                raster.rows(bad)

    def test_accepts_maximum_row_count_without_allocating_fixture_data(self):
        with tempfile.TemporaryFile() as file:
            file.truncate(raster.MAX_CASES * raster.INPUT_ROW_BYTES)
            with mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
                self.assertEqual(len(raster.rows(data)), raster.MAX_CASES)

    def test_truncated_and_extra_outputs_are_rejected(self):
        inputs = header() + bytes(raster.INPUT_ROW_BYTES - raster.HEADER_BYTES)
        output = bytes(raster.OUTPUT_BYTES)
        for name in ('original', 'native'):
            for invalid in (output[:-1], output + b'\0'):
                pair = {'original': output, 'native': output}
                pair[name] = invalid
                with self.subTest(name=name, length=len(invalid)), self.assertRaisesRegex(ValueError, name + ' output extent'):
                    raster.compare(inputs, **pair)

    def test_pixel_mismatch_report_has_exact_pixel_and_rgba(self):
        inputs = header(rgba=0x10203080) + bytes(raster.INPUT_ROW_BYTES - raster.HEADER_BYTES)
        original = bytearray(raster.OUTPUT_BYTES)
        native = bytearray(original)
        pixel = (17 * raster.WIDTH + 23) * 4
        native[pixel:pixel + 4] = bytes((0, 1, 127, 255))
        result = raster.compare(inputs, bytes(original), bytes(native))
        self.assertFalse(result['exact'])
        self.assertEqual((result['differing_pixels'], result['differing_channels'], result['max_abs_diff']),
                         (1, 3, 255))
        self.assertEqual(result['first_difference'], {
            'case': 0,
            'case_input': {'case': 0, 'mode': 2, 'mode_hex': '0x00000002',
                           'rgba': 0x10203080, 'rgba_hex': '0x10203080',
                           'progress': 0, 'progress_hex': '0x00000000', 'field_nonzero': 0},
            'x': 23, 'y': 17, 'channel': 'g',
            'original_rgba': [0, 0, 0, 0], 'native_rgba': [0, 1, 127, 255]})
        self.assertEqual(result['sha256']['native'], hashlib.sha256(native).hexdigest())

if __name__ == '__main__':
    unittest.main()
