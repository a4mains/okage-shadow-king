#!/usr/bin/env python3
"""Generate and verify bounded original/native game-variable reset fixtures.

Each 0x1020-byte row contains a 16-byte prefix, 0x1000 bytes of root storage,
and a 16-byte suffix. Only root[0:0xd78] is reset. No game or oracle is launched;
this checks the reset primitive, not authentic new-game lifecycle execution.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROW_SIZE = 0x1020
ROOT_OFFSET = 16
ROOT_SIZE = 0x1000
RESET_SIZE = 0xd78
MAX_ROWS = 16

def fixtures():
    result = bytearray()
    for number in range(4):
        root = (bytes([(0, 0xff, 0xa5)[number]]) * ROOT_SIZE
                if number < 3 else
                bytes((i * 73 + (i >> 8) * 19 + 0x31) & 255 for i in range(ROOT_SIZE)))
        # Distinct, nonzero sentinels identify rows even when the root is zero.
        prefix = bytes(1 + (number * 43 + i * 7) % 255 for i in range(16))
        suffix = bytes(1 + (number * 47 + i * 11 + 113) % 255 for i in range(16))
        result.extend(prefix + root + suffix)
    return bytes(result)

def rows(inputs):
    if not inputs or len(inputs) % ROW_SIZE or len(inputs) > MAX_ROWS * ROW_SIZE:
        raise ValueError('Game variable reset input extent: expected 1..16 whole 0x1020-byte rows')
    return [inputs[i:i + ROW_SIZE] for i in range(0, len(inputs), ROW_SIZE)]

def expected_row(row):
    if len(row) != ROW_SIZE:
        raise ValueError('Game variable reset row extent')
    # Independent final byte layout from 0x124f10, 0x124e98 and 0x125b08.
    # The latter's ASM stack stores are essential; its decompilation omits them.
    reset = bytearray(RESET_SIZE)
    reset[0xe] = 1
    reset[0x844:0x848] = b'Ari\0'       # Retail constant at 0x2237d0.
    reset[0x864:0x868] = b'???\0'       # Retail constant at 0x2237d8.
    for offset, value in ((0x51c, 1000), (0x570, 3), (0x588, 3)):
        struct.pack_into('<I', reset, offset, value)
    for slot, (argument2, argument3) in enumerate(
            ((0xaa, 1), (0xab, 5), (0xac, 10), (0xad, 25),
             (0xae, 30), (0xaf, 40), (0xb0, 1))):
        # Seven 0x50-byte records, with byte@0 and shorts@2,@4,@0xe.
        offset = 0x5c4 + slot * 0x50
        struct.pack_into('<BxHH8xH', reset, offset, 1, argument2, argument3, 5)
    return row[:ROOT_OFFSET] + reset + row[ROOT_OFFSET + RESET_SIZE:]

def location(offset):
    if offset < ROOT_OFFSET:
        return f'prefix canary+0x{offset:x}'
    root = offset - ROOT_OFFSET
    if root >= ROOT_SIZE:
        return f'suffix canary+0x{root - ROOT_SIZE:x}'
    if root >= RESET_SIZE:
        return f'preserved root tail+0x{root:x}'
    if 0x5c4 <= root < 0x7f4:
        return f'root+0x{root:x} record {(root - 0x5c4) // 0x50}+0x{(root - 0x5c4) % 0x50:x}'
    return f'root+0x{root:x}'

def compare(inputs, original, native):
    records = rows(inputs)
    for name, data in (('original', original), ('native', native)):
        if len(data) != len(inputs):
            raise ValueError(f'Game variable reset {name} output extent: '
                             f'expected {len(inputs)}, got {len(data)}')
    for number, row in enumerate(records):
        expected = expected_row(row)
        at = number * ROW_SIZE
        for name, data in (('original', original), ('native', native)):
            actual = data[at:at + ROW_SIZE]
            if actual != expected:
                first = next(i for i, (a, b) in enumerate(zip(actual, expected)) if a != b)
                raise ValueError(f'Game variable reset {name} row {number} {location(first)}: '
                                 f'expected {expected[first]:02x}, got {actual[first]:02x}')
    return {'rows': len(records), 'bytes_per_row': ROW_SIZE,
            'compared_bytes_per_output': len(inputs), 'reset_bytes_per_row': RESET_SIZE,
            'preserved_bytes_per_row': ROW_SIZE - RESET_SIZE,
            'exact': True, 'independent_expected_layout': True,
            'scope': 'Entire rows, including prefix/suffix canaries and untouched root tail; '
                     'reset primitive only, not whole new-game initialization.',
            'sha256': {name: hashlib.sha256(data).hexdigest() for name, data in
                       (('input', inputs), ('original', original), ('native', native))}}

def write_fresh(path, data):
    # Exclusive creation rejects existing files, directories and symlinks.
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as output:
        output.write(data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    generate = sub.add_parser('generate')
    generate.add_argument('output', type=Path)
    verify = sub.add_parser('verify')
    for name in ('input', 'original', 'native'):
        verify.add_argument(name, type=Path)
    verify.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.command == 'generate':
        data = fixtures()
        write_fresh(args.output, data)
        print(f'{len(rows(data))} rows; sha256 {hashlib.sha256(data).hexdigest()}')
    else:
        result = compare(args.input.read_bytes(), args.original.read_bytes(), args.native.read_bytes())
        result['paths'] = {name: str(getattr(args, name)) for name in ('input', 'original', 'native')}
        output = json.dumps(result, indent=2) + '\n'
        if args.report:
            write_fresh(args.report, output.encode())
        print(output, end='')

if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc
