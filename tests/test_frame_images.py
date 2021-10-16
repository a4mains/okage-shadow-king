import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

spec = importlib.util.spec_from_file_location(
    'compare_frame_images', Path(__file__).resolve().parents[1] / 'tools/compare_frame_images.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def png_with_color_type(width, height, color_type, rows):
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[color_type]
    scan = b''.join(b'\0' + row for row in rows)
    ihdr = struct.pack('>IIBBBBB', width, height, 8, color_type, 0, 0, 0)
    return (module.PNG_SIGNATURE + module._chunk(b'IHDR', ihdr) +
            module._chunk(b'IDAT', zlib.compress(scan)) + module._chunk(b'IEND', b''))

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c

def filtered_png(rows, filters, *, color_type=2, palette=None, transparency=None):
    """Build tiny PNG rows independently using the PNG filter definitions."""
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    previous = bytes(len(rows[0]))
    scanlines = bytearray()
    for row, filter_type in zip(rows, filters):
        encoded = bytearray(len(row))
        for index, value in enumerate(row):
            left = row[index - channels] if index >= channels else 0
            above = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = above
            elif filter_type == 3:
                predictor = (left + above) // 2
            else:
                predictor = paeth(left, above, upper_left)
            encoded[index] = (value - predictor) & 255
        scanlines.extend((filter_type,))
        scanlines.extend(encoded)
        previous = row
    width = len(rows[0]) // channels
    ihdr = struct.pack('>IIBBBBB', width, len(rows), 8, color_type, 0, 0, 0)
    output = module.PNG_SIGNATURE + module._chunk(b'IHDR', ihdr)
    if palette is not None:
        output += module._chunk(b'PLTE', palette)
    if transparency is not None:
        output += module._chunk(b'tRNS', transparency)
    return (output + module._chunk(b'IDAT', zlib.compress(bytes(scanlines))) +
            module._chunk(b'IEND', b''))

class FrameImageComparisonTests(unittest.TestCase):
    def test_rgba_metrics_and_absolute_rgb_difference(self):
        original = (2, 1, bytes((10, 20, 30, 40, 50, 60, 70, 80)))
        native = (2, 1, bytes((20, 20, 30, 40, 50, 60, 70, 180)))
        metrics, difference = module.compare_rgba(original, native)
        self.assertEqual(metrics['exact_different_pixel_count'], 2)
        self.assertAlmostEqual(metrics['rgb_mae'], 10 / 6)
        self.assertEqual(metrics['rgb_max_absolute_difference'], 10)
        self.assertEqual(metrics['alpha_different_pixel_count'], 1)
        self.assertEqual(metrics['alpha_mae'], 50)
        self.assertEqual(metrics['alpha_max_absolute_difference'], 100)
        self.assertEqual(difference, bytes((10, 0, 0, 255, 0, 0, 0, 255)))

    def test_rgb_png_converts_to_rgba(self):
        encoded = png_with_color_type(1, 1, 2, [bytes((7, 11, 13))])
        self.assertEqual(module.decode_png(encoded), (1, 1, bytes((7, 11, 13, 255))))

    def test_all_five_png_row_filters(self):
        first = bytes((10, 20, 30, 40, 50, 60))
        second = bytes((12, 25, 33, 38, 49, 70))
        expected = bytes((10, 20, 30, 255, 40, 50, 60, 255,
                          12, 25, 33, 255, 38, 49, 70, 255))
        for filter_type in range(5):
            with self.subTest(filter_type=filter_type):
                encoded = filtered_png([first, second], [0, filter_type])
                self.assertEqual(module.decode_png(encoded), (2, 2, expected))

    def test_indexed_png_transparency_and_bad_crc(self):
        indexed = filtered_png([bytes((0, 1))], [0], color_type=3,
                               palette=bytes((255, 0, 0, 0, 255, 0)),
                               transparency=bytes((0, 128)))
        self.assertEqual(module.decode_png(indexed),
                         (2, 1, bytes((255, 0, 0, 0, 0, 255, 0, 128))))

        damaged = bytearray(indexed)
        offset = len(module.PNG_SIGNATURE)
        for _ in range(3):
            length = struct.unpack_from('>I', damaged, offset)[0]
            offset += 12 + length
        data_length = struct.unpack_from('>I', damaged, offset)[0]
        crc_offset = offset + 8 + data_length
        damaged[crc_offset] ^= 1
        with self.assertRaisesRegex(ValueError, 'bad PNG CRC'):
            module.decode_png(bytes(damaged))

    def test_rejects_bad_transparency_lengths_and_trailing_bytes(self):
        invalid_cases = (
            (0, bytes((10,)), bytes((0,)), None),
            (2, bytes((10, 20, 30)), bytes((0, 10)), None),
            (3, bytes((0,)), bytes((0, 1)), bytes((1, 2, 3))),
            (3, bytes((0,)), b'', bytes((1, 2, 3))),
            (6, bytes((1, 2, 3, 4)), bytes((0, 0)), None),
        )
        for color_type, row, transparency, palette in invalid_cases:
            with self.subTest(color_type=color_type):
                encoded = filtered_png([row], [0], color_type=color_type,
                                       palette=palette, transparency=transparency)
                with self.assertRaisesRegex(ValueError, 'invalid PNG tRNS'):
                    module.decode_png(encoded)

        valid = png_with_color_type(1, 1, 2, [bytes((1, 2, 3))])
        with self.assertRaisesRegex(ValueError, 'trailing bytes after PNG IEND'):
            module.decode_png(valid + b'extra')

    def test_explicit_native_crop_and_output_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = root / 'reference.png'
            native = root / 'native.png'
            reference_pixels = b''.join(bytes((x * 10, y * 20, 30, 255))
                                        for y in range(2) for x in range(2))
            original.write_bytes(module.encode_png(2, 2, reference_pixels))

            full_native = bytearray(4 * 3 * 4)
            for y in range(3):
                for x in range(4):
                    at = (y * 4 + x) * 4
                    full_native[at:at + 4] = bytes((max(0, x - 1) * 10,
                                                    max(0, y - 1) * 20, 30, 255))
            native.write_bytes(module.encode_png(4, 3, bytes(full_native)))
            output = root / 'comparison'
            manifest = module.compare_files(original, native, output, (1, 1, 2, 2))

            self.assertEqual(manifest['declared_native_region'], {
                'x': 1, 'y': 1, 'width': 2, 'height': 2,
            })
            self.assertEqual(manifest['compared_rgba_extent'], {'width': 2, 'height': 2})
            self.assertEqual(manifest['metrics']['exact_different_pixel_count'], 0)
            self.assertFalse(manifest['temporal_alignment'])
            self.assertFalse(manifest['threshold_or_pass_assertion'])
            side = module.decode_png((output / 'side-by-side.png').read_bytes())
            diff = module.decode_png((output / 'absolute-rgb-difference.png').read_bytes())
            self.assertEqual(side[:2], (4, 32))
            self.assertEqual(diff[:2], (2, 2))
            self.assertEqual(json.loads((output / 'comparison.json').read_text()), manifest)

    def test_rejects_mismatched_extents_and_crop_bounds(self):
        image = (2, 2, bytes((0, 0, 0, 255)) * 4)
        with self.assertRaisesRegex(ValueError, 'extents differ'):
            module.compare_rgba(image, (1, 4, bytes((0, 0, 0, 255)) * 4))
        with self.assertRaisesRegex(ValueError, 'outside image bounds'):
            module.crop_rgba(*image, (1, 1, 2, 2))
        with self.assertRaisesRegex(ValueError, 'outside image bounds'):
            module.crop_rgba(*image, (-1, 0, 1, 1))

    def test_refuses_existing_directory_and_symlink_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'source.png'
            source.write_bytes(module.encode_png(1, 1, bytes((1, 2, 3, 255))))
            existing = root / 'existing'
            existing.mkdir()
            with self.assertRaises(FileExistsError):
                module.compare_files(source, source, existing)

            dangling = root / 'dangling'
            dangling.symlink_to(root / 'missing-target')
            with self.assertRaises(FileExistsError):
                module.compare_files(source, source, dangling)
            self.assertTrue(dangling.is_symlink())

if __name__ == '__main__':
    unittest.main()
