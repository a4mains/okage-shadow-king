#!/usr/bin/env python3
"""Create a numeric, explicitly paired comparison of two PNG frame images.

This is a diagnostic image comparison, not a fidelity pass. It does not align
time, infer viewports, resize, or choose thresholds. `--native-region` is an
explicit crop in native-image coordinates; it is useful when a native capture
has a diagnostic panel beside the game viewport (for example, x=0,y=0,w=640,h=224).

The implementation uses Python's standard library and accepts noninterlaced,
8-bit PNG grayscale, RGB, indexed, grayscale-alpha, and RGBA images. Metrics
compare raw encoded 8-bit samples; no ICC, gamma, transfer-function, or other
color-space transforms are applied.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
_GLYPHS = {
    ' ': (0, 0, 0, 0, 0, 0, 0), 'A': (14, 17, 17, 31, 17, 17, 17),
    'B': (30, 17, 17, 30, 17, 17, 30), 'C': (14, 17, 16, 16, 16, 17, 14),
    'D': (30, 17, 17, 17, 17, 17, 30), 'E': (31, 16, 16, 30, 16, 16, 31),
    'F': (31, 16, 16, 30, 16, 16, 16), 'G': (14, 17, 16, 23, 17, 17, 15),
    'H': (17, 17, 17, 31, 17, 17, 17), 'I': (14, 4, 4, 4, 4, 4, 14),
    'J': (7, 2, 2, 2, 18, 18, 12), 'K': (17, 18, 20, 24, 20, 18, 17),
    'L': (16, 16, 16, 16, 16, 16, 31), 'M': (17, 27, 21, 21, 17, 17, 17),
    'N': (17, 25, 21, 19, 17, 17, 17), 'O': (14, 17, 17, 17, 17, 17, 14),
    'P': (30, 17, 17, 30, 16, 16, 16), 'Q': (14, 17, 17, 17, 21, 18, 13),
    'R': (30, 17, 17, 30, 20, 18, 17), 'S': (15, 16, 16, 14, 1, 1, 30),
    'T': (31, 4, 4, 4, 4, 4, 4), 'U': (17, 17, 17, 17, 17, 17, 14),
    'V': (17, 17, 17, 17, 17, 10, 4), 'W': (17, 17, 17, 21, 21, 21, 10),
    'X': (17, 17, 10, 4, 10, 17, 17), 'Y': (17, 17, 10, 4, 4, 4, 4),
    'Z': (31, 1, 2, 4, 8, 16, 31), '0': (14, 17, 19, 21, 25, 17, 14),
    '1': (4, 12, 4, 4, 4, 4, 14), '2': (14, 17, 1, 2, 4, 8, 31),
    '3': (30, 1, 1, 14, 1, 1, 30), '4': (2, 6, 10, 18, 31, 2, 2),
    '5': (31, 16, 16, 30, 1, 1, 30), '6': (14, 16, 16, 30, 17, 17, 14),
    '7': (31, 1, 2, 4, 8, 8, 8), '8': (14, 17, 17, 14, 17, 17, 14),
    '9': (14, 17, 17, 15, 1, 1, 14), ':': (0, 4, 4, 0, 4, 4, 0),
    '.': (0, 0, 0, 0, 0, 12, 12), ',': (0, 0, 0, 0, 4, 4, 8),
    '-': (0, 0, 0, 31, 0, 0, 0), '_': (0, 0, 0, 0, 0, 0, 31),
    '/': (1, 2, 2, 4, 8, 8, 16), '[': (14, 8, 8, 8, 8, 8, 14),
    ']': (14, 2, 2, 2, 2, 2, 14), '(': (2, 4, 8, 8, 8, 4, 2),
    ')': (8, 4, 2, 2, 2, 4, 8), '?': (14, 17, 1, 2, 4, 0, 4),
}

def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c

def decode_png(data):
    """Decode a common noninterlaced 8-bit PNG to (width, height, RGBA bytes)."""
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError('input is not a PNG')
    offset = len(PNG_SIGNATURE)
    chunks = []
    palette = transparency = None
    width = height = bit_depth = color_type = interlace = None
    saw_end = False
    while offset < len(data):
        if len(data) - offset < 12:
            raise ValueError('truncated PNG chunk')
        length = struct.unpack_from('>I', data, offset)[0]
        kind = data[offset + 4:offset + 8]
        end = offset + 12 + length
        if end > len(data):
            raise ValueError('truncated PNG chunk payload')
        payload = data[offset + 8:offset + 8 + length]
        expected_crc = struct.unpack_from('>I', data, offset + 8 + length)[0]
        if zlib.crc32(kind + payload) & 0xffffffff != expected_crc:
            raise ValueError(f'bad PNG CRC in {kind.decode("ascii", "replace")}')
        offset = end
        if kind == b'IHDR':
            if width is not None or len(payload) != 13:
                raise ValueError('invalid PNG IHDR')
            width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack('>IIBBBBB', payload)
            if not width or not height:
                raise ValueError('PNG dimensions must be positive')
            if bit_depth != 8 or compression or filtering or interlace:
                raise ValueError('only noninterlaced 8-bit PNG images are supported')
            if color_type not in (0, 2, 3, 4, 6):
                raise ValueError(f'unsupported PNG color type {color_type}')
        elif kind == b'PLTE':
            palette = payload
        elif kind == b'tRNS':
            transparency = payload
        elif kind == b'IDAT':
            chunks.append(payload)
        elif kind == b'IEND':
            saw_end = True
            break
        elif kind[0] & 0x20 == 0 and kind not in (b'IHDR',):
            raise ValueError(f'unsupported critical PNG chunk {kind!r}')
    if width is None or not saw_end:
        raise ValueError('PNG is missing IHDR or IEND')
    if offset != len(data):
        raise ValueError('trailing bytes after PNG IEND')
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    if transparency is not None:
        valid_transparency = (
            (color_type == 0 and len(transparency) == 2) or
            (color_type == 2 and len(transparency) == 6) or
            (color_type == 3 and palette is not None and
             0 < len(transparency) <= len(palette) // 3)
        )
        if not valid_transparency:
            raise ValueError('invalid PNG tRNS for its color type/palette')
    stride = width * channels
    try:
        raw = zlib.decompress(b''.join(chunks))
    except zlib.error as error:
        raise ValueError(f'invalid PNG image data: {error}') from error
    if len(raw) != height * (stride + 1):
        raise ValueError('PNG decompressed extent does not match dimensions')

    rows = []
    cursor = 0
    previous = bytearray(stride)
    for _ in range(height):
        filter_type = raw[cursor]
        scan = bytearray(raw[cursor + 1:cursor + 1 + stride])
        cursor += stride + 1
        if filter_type > 4:
            raise ValueError(f'unsupported PNG row filter {filter_type}')
        for index in range(stride):
            left = scan[index - channels] if index >= channels else 0
            above = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filter_type == 1:
                scan[index] = (scan[index] + left) & 255
            elif filter_type == 2:
                scan[index] = (scan[index] + above) & 255
            elif filter_type == 3:
                scan[index] = (scan[index] + ((left + above) // 2)) & 255
            elif filter_type == 4:
                scan[index] = (scan[index] + _paeth(left, above, upper_left)) & 255
        rows.append(scan)
        previous = scan

    rgba = bytearray(width * height * 4)
    destination = 0
    for row in rows:
        for x in range(width):
            at = x * channels
            if color_type == 6:
                pixel = row[at:at + 4]
            elif color_type == 2:
                r, g, b = row[at:at + 3]
                alpha = 0 if transparency and (r, g, b) == struct.unpack('>HHH', transparency) else 255
                pixel = bytes((r, g, b, alpha))
            elif color_type == 0:
                gray = row[at]
                alpha = 0 if transparency and gray == struct.unpack('>H', transparency)[0] else 255
                pixel = bytes((gray, gray, gray, alpha))
            elif color_type == 4:
                gray, alpha = row[at:at + 2]
                pixel = bytes((gray, gray, gray, alpha))
            else:
                if palette is None or len(palette) % 3:
                    raise ValueError('indexed PNG has an invalid/missing palette')
                palette_index = row[at]
                p = palette_index * 3
                if p + 3 > len(palette):
                    raise ValueError('indexed PNG palette index is out of range')
                alpha = transparency[palette_index] if transparency and palette_index < len(transparency) else 255
                pixel = bytes((*palette[p:p + 3], alpha))
            rgba[destination:destination + 4] = pixel
            destination += 4
    return width, height, bytes(rgba)

def _chunk(kind, payload):
    return (struct.pack('>I', len(payload)) + kind + payload +
            struct.pack('>I', zlib.crc32(kind + payload) & 0xffffffff))

def encode_png(width, height, rgba):
    if width <= 0 or height <= 0 or len(rgba) != width * height * 4:
        raise ValueError('invalid RGBA image extent')
    stride = width * 4
    scanlines = b''.join(b'\0' + rgba[y * stride:(y + 1) * stride] for y in range(height))
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    return (PNG_SIGNATURE + _chunk(b'IHDR', ihdr) +
            _chunk(b'IDAT', zlib.compress(scanlines, 9)) + _chunk(b'IEND', b''))

def crop_rgba(width, height, rgba, region):
    if region is None:
        return width, height, rgba
    x, y, crop_width, crop_height = region
    if min(x, y) < 0 or crop_width <= 0 or crop_height <= 0 or x + crop_width > width or y + crop_height > height:
        raise ValueError(f'native region {region} is outside image bounds {width}x{height}')
    rows = []
    for row in range(y, y + crop_height):
        start = (row * width + x) * 4
        rows.append(rgba[start:start + crop_width * 4])
    return crop_width, crop_height, b''.join(rows)

def _label(rgba, width, height, x, y, text, max_width):
    cursor = x
    for character in text.upper():
        if cursor + 6 > x + max_width:
            break
        glyph = _GLYPHS.get(character, _GLYPHS['?'])
        for gy, bits in enumerate(glyph):
            py = y + gy
            if py >= height:
                continue
            for gx in range(5):
                if bits & (1 << (4 - gx)):
                    px = cursor + gx
                    at = (py * width + px) * 4
                    rgba[at:at + 4] = b'\xff\xff\xff\xff'
        cursor += 6

def side_by_side(width, height, original, native,
                 original_name, native_name, original_view, native_view):
    band = 30
    combined_width = width * 2
    combined_height = height + band
    out = bytearray(combined_width * combined_height * 4)
    for y in range(combined_height):
        for x in range(combined_width):
            at = (y * combined_width + x) * 4
            out[at:at + 4] = b'\x20\x20\x20\xff' if y < band else b'\0\0\0\xff'
    for y in range(height):
        left = y * width * 4
        right = (y + band) * combined_width * 4
        out[right:right + width * 4] = original[left:left + width * 4]
        out[right + width * 4:right + 2 * width * 4] = native[left:left + width * 4]
    _label(out, combined_width, band, 3, 3, f'ORIGINAL {original_name}', max(0, width - 6))
    _label(out, combined_width, band, width + 3, 3, f'NATIVE {native_name}', max(0, width - 6))
    _label(out, combined_width, band, 3, 19, f'ORIGINAL {original_view}', max(0, width - 6))
    _label(out, combined_width, band, width + 3, 19, f'NATIVE {native_view}', max(0, width - 6))
    return combined_width, combined_height, bytes(out)

def compare_rgba(original, native):
    ow, oh, a = original
    nw, nh, b = native
    if (ow, oh) != (nw, nh):
        raise ValueError(f'paired image extents differ: original {ow}x{oh}, native {nw}x{nh}')
    count = ow * oh
    rgb_sum = rgb_max = alpha_sum = alpha_max = alpha_pixels = different_pixels = 0
    for pixel in range(count):
        start = pixel * 4
        channel_diffs = [abs(a[start + c] - b[start + c]) for c in range(4)]
        rgb_sum += sum(channel_diffs[:3])
        rgb_max = max(rgb_max, *channel_diffs[:3])
        alpha_sum += channel_diffs[3]
        alpha_max = max(alpha_max, channel_diffs[3])
        alpha_pixels += channel_diffs[3] != 0
        different_pixels += any(channel_diffs)
    diff = bytearray(count * 4)
    for pixel in range(count):
        start = pixel * 4
        diff[start:start + 4] = bytes((abs(a[start] - b[start]), abs(a[start + 1] - b[start + 1]),
                                       abs(a[start + 2] - b[start + 2]), 255))
    return {
        'width': ow, 'height': oh,
        'exact_different_pixel_count': different_pixels,
        'rgb_mae': rgb_sum / (count * 3),
        'rgb_max_absolute_difference': rgb_max,
        'alpha_different_pixel_count': alpha_pixels,
        'alpha_mae': alpha_sum / count,
        'alpha_max_absolute_difference': alpha_max,
    }, bytes(diff)

def compare_files(original_path, native_path, output_directory, native_region=None):
    original_path, native_path = Path(original_path), Path(native_path)
    original_bytes = original_path.read_bytes()
    native_bytes = native_path.read_bytes()
    original = decode_png(original_bytes)
    native_full = decode_png(native_bytes)
    native = crop_rgba(*native_full, native_region)
    metrics, difference = compare_rgba(original, native)
    width, height = metrics['width'], metrics['height']
    native_view = ('{}X{}'.format(native_full[0], native_full[1]) if native_region is None else
                   'CROP {},{} {}X{}'.format(*native_region))
    side_width, side_height, paired = side_by_side(
        width, height, original[2], native[2],
        original_path.name, native_path.name,
        '{}X{}'.format(original[0], original[1]), native_view)
    manifest = {
        'purpose': 'Explicitly paired numeric diagnostic; not a fidelity pass.',
        'temporal_alignment': False,
        'threshold_or_pass_assertion': False,
        'automatic_resize_or_crop': False,
        'alpha_note': 'RGB metrics exclude alpha; alpha differences are reported separately.',
        'sample_basis': ('Raw encoded 8-bit channel samples after PNG decoding to RGBA; '
                         'no ICC profile, gamma, transfer-function, or color-space transforms are applied.'),
        'inputs': {
            'original': {'path': str(original_path), 'sha256': hashlib.sha256(original_bytes).hexdigest(),
                         'width': original[0], 'height': original[1]},
            'native': {'path': str(native_path), 'sha256': hashlib.sha256(native_bytes).hexdigest(),
                       'width': native_full[0], 'height': native_full[1]},
        },
        'declared_native_region': (None if native_region is None else {
            'x': native_region[0], 'y': native_region[1],
            'width': native_region[2], 'height': native_region[3],
        }),
        'compared_rgba_extent': {'width': width, 'height': height},
        'metrics': metrics,
        'outputs': {
            'side_by_side': {'file': 'side-by-side.png', 'width': side_width, 'height': side_height},
            'absolute_rgb_difference': {'file': 'absolute-rgb-difference.png',
                                        'width': width, 'height': height},
        },
    }
    output_directory = Path(output_directory)
    output_directory.mkdir(parents=True, exist_ok=False)
    (output_directory / 'side-by-side.png').write_bytes(encode_png(side_width, side_height, paired))
    (output_directory / 'absolute-rgb-difference.png').write_bytes(encode_png(width, height, difference))
    (output_directory / 'comparison.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest

def _parse_region(value):
    try:
        parts = tuple(int(part) for part in value.split(','))
    except ValueError as error:
        raise argparse.ArgumentTypeError('region must be x,y,width,height') from error
    if len(parts) != 4:
        raise argparse.ArgumentTypeError('region must be x,y,width,height')
    x, y, width, height = parts
    if x < 0 or y < 0 or width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError('region requires x,y >= 0 and width,height > 0')
    return parts

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, required=True, help='Original/reference PNG')
    parser.add_argument('--native', type=Path, required=True, help='Native PNG')
    parser.add_argument('--output', type=Path, required=True, help='New output directory')
    parser.add_argument('--native-region', type=_parse_region,
                        help='Explicit native crop x,y,width,height; e.g. 0,0,640,224')
    args = parser.parse_args()
    try:
        report = compare_files(args.original, args.native, args.output, args.native_region)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(report['metrics'], sort_keys=True))

if __name__ == '__main__':
    main()
