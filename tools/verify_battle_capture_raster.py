#!/usr/bin/env python3
"""Generate synthetic battle-capture raster inputs and compare raw RGBA exactly.

Each input row contains eight little-endian u32 words followed by three
640x256 RGBA images: current destination, history, and fixed capture. Outputs
contain one 640x224 RGBA image per row. The fixture data is synthetic and
provides no original-game oracle or evidence of capture fidelity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

WIDTH = 640
INPUT_HEIGHT = 256
OUTPUT_HEIGHT = 224
CHANNELS = 4
PIXELS = WIDTH * OUTPUT_HEIGHT
IMAGE_BYTES = WIDTH * INPUT_HEIGHT * CHANNELS
OUTPUT_BYTES = PIXELS * CHANNELS
HEADER_BYTES = 32
INPUT_ROW_BYTES = HEADER_BYTES + 3 * IMAGE_BYTES
MAX_CASES = 16
HEADER_FIELDS = ('mode', 'rgba', 'progress', 'field_nonzero', 'reserved0',
                 'reserved1', 'reserved2', 'reserved3')
CHANNEL_NAMES = ('r', 'g', 'b', 'a')

def fixture_cases():
    """Return <=16 explicit synthetic cases spanning raster branches."""
    return [
        {'name': 'sprite_field_zero_alpha0', 'mode': 2, 'rgba': 0x00808080,
         'progress': 0, 'field_nonzero': 0, 'pattern': 'ramp'},
        {'name': 'sprite_field_one_alpha64', 'mode': 2, 'rgba': 0x40808080,
         'progress': 0, 'field_nonzero': 1, 'pattern': 'checker'},
        {'name': 'sprite_half_scale_alpha128', 'mode': 2,
         'rgba': 0x80808080, 'progress': 0x00008000, 'field_nonzero': 0,
         'pattern': 'impulse'},
        {'name': 'sprite_scale_1_25_alpha255', 'mode': 2,
         'rgba': 0xFF808080, 'progress': 0xffffc000, 'field_nonzero': 1,
         'pattern': 'edges'},
        {'name': 'fan_rotation_45', 'mode': 0x20002, 'rgba': 0x80808080,
         'progress': 0, 'field_nonzero': 0, 'pattern': 'checker'},
        {'name': 'fan_rotation_90_scale_0_75', 'mode': 0x40002, 'rgba': 0x80808080,
         'progress': 0x00004000, 'field_nonzero': 1, 'pattern': 'ramp'},
        {'name': 'fan_rotation_22_5_scale_0_25', 'mode': 0x10002, 'rgba': 0x80808080,
         'progress': 0x0000c000, 'field_nonzero': 0, 'pattern': 'impulse'},
        {'name': 'rgb_modulation', 'mode': 0x20002, 'rgba': 0x8060c040,
         'progress': 0x00004000, 'field_nonzero': 1, 'pattern': 'edges'},
    ]

def _pixel(pattern, role, x, y, case_index):
    """A deterministic RGBA texel; each source role has a distinct signature."""
    role_bias = (37 * role + 19 * case_index) & 255
    if pattern == 'ramp':
        rgb = ((x + role_bias) & 255, (y * 3 + role_bias) & 255,
               (x // 2 + y + role_bias) & 255)
        alpha = (x + 2 * y + role * 61 + case_index) % 256
    elif pattern == 'checker':
        cell = ((x // (3 + role)) ^ (y // (5 + role))) & 1
        rgb = ((23 + role_bias + cell * 211) & 255,
               (91 + role_bias + cell * 127) & 255,
               (173 + role_bias + cell * 67) & 255)
        alpha = (0, 1, 127, 128, 255)[(x + y + role + case_index) % 5]
    elif pattern == 'impulse':
        hit = x == (7 + role * 31 + case_index * 13) % WIDTH and y == (11 + role * 17) % INPUT_HEIGHT
        rgb = ((255, 0, 255) if hit else ((role_bias, x & 255, y & 255)))
        alpha = 255 if hit else ((x * 7 + y * 11 + role_bias) & 255)
    elif pattern == 'edges':
        edge = x in (0, WIDTH - 1, 319, 320) or y in (0, OUTPUT_HEIGHT - 1, 224, 255)
        rgb = ((255, 255, 0) if edge else ((x + role_bias) & 255,
               (y + role_bias * 3) & 255, (x ^ y ^ role_bias) & 255))
        alpha = (0, 1, 127, 128, 255)[(x * 3 + y + role) % 5]
    else:
        raise ValueError(f'unknown raster pattern: {pattern}')
    return (*rgb, alpha)

def _image(pattern, role, case_index):
    image = bytearray(IMAGE_BYTES)
    offset = 0
    for y in range(INPUT_HEIGHT):
        for x in range(WIDTH):
            image[offset:offset + 4] = bytes(_pixel(pattern, role, x, y, case_index))
            offset += 4
    return image

def fixtures():
    """Build the full binary input; at most 16 rows fit the original 32 MiB cap."""
    cases = fixture_cases()
    if not 1 <= len(cases) <= MAX_CASES:
        raise ValueError(f'Battle raster fixture count must be 1..{MAX_CASES}')
    result = bytearray()
    for index, case in enumerate(cases):
        result.extend(struct.pack('<8I', case['mode'], case['rgba'], case['progress'],
                                  case['field_nonzero'], 0, 0, 0, 0))
        for role in range(3):
            result.extend(_image(case['pattern'], role, index))
    return bytes(result)

def rows(inputs):
    if (not inputs or len(inputs) % INPUT_ROW_BYTES or
            len(inputs) > MAX_CASES * INPUT_ROW_BYTES):
        raise ValueError(f'Battle raster input extent: expected 1..{MAX_CASES} whole {INPUT_ROW_BYTES}-byte rows')
    result = []
    for index, offset in enumerate(range(0, len(inputs), INPUT_ROW_BYTES)):
        values = struct.unpack_from('<8I', inputs, offset)
        if values[3] not in (0, 1) or any(values[4:]):
            raise ValueError(f'Battle raster case {index} header flags/reserved words')
        mode, rgba, progress, field_nonzero = values[:4]
        result.append({'case': index, 'mode': mode, 'mode_hex': f'0x{mode:08x}',
                       'rgba': rgba, 'rgba_hex': f'0x{rgba:08x}',
                       'progress': progress, 'progress_hex': f'0x{progress:08x}',
                       'field_nonzero': field_nonzero})
    return result

def _first_difference(original, native, cases):
    for byte, (a, b) in enumerate(zip(original, native)):
        if a != b:
            case_index, pixel_byte = divmod(byte, OUTPUT_BYTES)
            pixel_index, channel = divmod(pixel_byte, CHANNELS)
            y, x = divmod(pixel_index, WIDTH)
            base = case_index * OUTPUT_BYTES + pixel_index * CHANNELS
            return {'case': case_index, 'case_input': cases[case_index],
                    'x': x, 'y': y, 'channel': CHANNEL_NAMES[channel],
                    'original_rgba': list(original[base:base + 4]),
                    'native_rgba': list(native[base:base + 4])}
    return None

def compare(inputs, original, native):
    cases = rows(inputs)
    expected_size = len(cases) * OUTPUT_BYTES
    for name, output in (('original', original), ('native', native)):
        if len(output) != expected_size:
            raise ValueError(f'Battle raster {name} output extent: expected {expected_size}, got {len(output)}')
    differing_pixels = differing_channels = max_abs_diff = 0
    for start in range(0, expected_size, CHANNELS):
        diffs = [abs(original[start + c] - native[start + c]) for c in range(CHANNELS)]
        changed = sum(delta != 0 for delta in diffs)
        if changed:
            differing_pixels += 1
            differing_channels += changed
            max_abs_diff = max(max_abs_diff, *diffs)
    mismatch = _first_difference(original, native, cases)
    return {'case_count': len(cases), 'width': WIDTH, 'height': OUTPUT_HEIGHT,
            'expected_output_bytes': expected_size, 'compared_bytes': expected_size,
            'exact': mismatch is None, 'differing_pixels': differing_pixels,
            'differing_channels': differing_channels, 'max_abs_diff': max_abs_diff,
            'first_difference': mismatch,
            'sha256': {name: hashlib.sha256(data).hexdigest()
                       for name, data in (('input', inputs), ('original', original), ('native', native))},
            'scope': __doc__.strip()}

def write_fresh(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as output:
        output.write(data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    generate = commands.add_parser('generate')
    generate.add_argument('output', type=Path)
    generate.add_argument('metadata', type=Path)
    verify = commands.add_parser('verify')
    for name in ('input', 'original', 'native'):
        verify.add_argument(name, type=Path)
    verify.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'generate':
        data = fixtures()
        write_fresh(args.output, data)
        case_rows = rows(data)
        result = {'case_count': len(case_rows), 'input_row_bytes': INPUT_ROW_BYTES,
                  'input_bytes': len(data), 'max_input_bytes': MAX_CASES * INPUT_ROW_BYTES,
                  'sha256': hashlib.sha256(data).hexdigest(),
                  'cases': [{**fixture_cases()[i], **row} for i, row in enumerate(case_rows)],
                  'scope': __doc__.strip()}
        write_fresh(args.metadata, (json.dumps(result, indent=2) + '\n').encode())
    else:
        inputs = args.input.read_bytes()
        result = compare(inputs, args.original.read_bytes(), args.native.read_bytes())
        result['paths'] = {name: str(getattr(args, name)) for name in ('input', 'original', 'native')}
        write_fresh(args.report, (json.dumps(result, indent=2) + '\n').encode())
    print(json.dumps(result, indent=2))
    if args.command == 'verify' and not result['exact']:
        raise SystemExit(1)

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from error
