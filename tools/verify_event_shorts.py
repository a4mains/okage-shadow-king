#!/usr/bin/env python3
"""Generate and verify bounded synthetic signed16 event-bank primitive cases.

No emulator is launched. Original outputs must be produced separately by the
research oracle; native code never receives them as runtime inputs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

INPUT_ROW = 12
STORAGE_SIZE = 0x1000
OUTPUT_ROW = 4 + STORAGE_SIZE
MAX_ROWS = 1024

def fixtures():
    records = []

    def append(op, bank, index, value=0):
        records.append((op, bank << 16 | index, value & 0xffffffff))

    for bank in range(8, 16):
        for index in (0, 7, 39):
            append(0, bank, index)
            for value in (0, 32767, 32768, 65535, 65536, -1, -32769,
                          -2147483648, 2147483647):
                append(1, bank, index, value)
                append(2, bank, index, 1)
                append(2, bank, index, -1)
    append(1, 14, 7, 105)
    for index in (6, 7, 8):
        append(0, 14, index)
    return b''.join(struct.pack('<3I', *record) for record in records)

def rows(inputs):
    if not inputs or len(inputs) % INPUT_ROW or len(inputs) > MAX_ROWS * INPUT_ROW:
        raise ValueError('Event short input extent')
    records = list(struct.iter_unpack('<3I', inputs))
    for number, (op, selector, _) in enumerate(records):
        bank, index = selector >> 16, selector & 0xffff
        if op > 2 or not 8 <= bank <= 15 or index >= 40:
            raise ValueError(f'Unsupported event short input row {number}')
    return records

def compare(inputs, original, native):
    records = rows(inputs)
    size = len(records) * OUTPUT_ROW
    for name, data in (('original', original), ('native', native)):
        if len(data) != size:
            raise ValueError(f'Event short {name} output extent: expected {size}, got {len(data)}')
    storage = bytearray([0xa5] * STORAGE_SIZE)
    for number, (op, selector, value) in enumerate(records):
        bank, index = selector >> 16, selector & 0xffff
        offset = 0x5c4 + (bank - 8) * 0x50 + index * 2
        previous = int.from_bytes(storage[offset:offset + 2], 'little', signed=True)
        if op:
            result = (value + (previous if op == 2 else 0)) % 65536
            storage[offset:offset + 2] = result.to_bytes(2, 'little')
        signed = int.from_bytes(storage[offset:offset + 2], 'little', signed=True)
        expected = signed.to_bytes(4, 'little', signed=True) + storage
        at = number * OUTPUT_ROW
        for name, data in (('original', original), ('native', native)):
            actual = data[at:at + OUTPUT_ROW]
            if actual != expected:
                first = next(i for i, (a, b) in enumerate(zip(actual, expected)) if a != b)
                location = 'getter' if first < 4 else f'backing+0x{first - 4:x}'
                raise ValueError(f'Event short {name} row {number} {location}: '
                                 f'expected {expected[first]:02x}, got {actual[first]:02x}')
    return {'rows': len(records), 'bytes_per_row': OUTPUT_ROW,
            'compared_bytes': size, 'exact': True, 'arithmetic_and_full_backing': True,
            'sha256': {name: hashlib.sha256(data).hexdigest()
                       for name, data in (('input', inputs), ('original', original), ('native', native))}}

def write_exact(path, data):
    if path.is_symlink():
        raise ValueError(f'Refusing symlink output {path}')
    if path.exists():
        if path.read_bytes() != data:
            raise ValueError(f'Refusing different existing output {path}')
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)

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
        write_exact(args.output, data)
        print(f'{len(rows(data))} rows; sha256 {hashlib.sha256(data).hexdigest()}')
    else:
        result = compare(args.input.read_bytes(), args.original.read_bytes(), args.native.read_bytes())
        result['paths'] = {name: str(getattr(args, name)) for name in ('input', 'original', 'native')}
        output = json.dumps(result, indent=2) + '\n'
        if args.report:
            write_exact(args.report, output.encode())
        print(output, end='')

if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc
