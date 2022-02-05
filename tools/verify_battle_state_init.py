#!/usr/bin/env python3
"""Generate and compare two complete guarded battle-initializer snapshots.

Only bounded initialization helpers are covered, not battle progression. This
script never launches a reference VM and does not establish producer provenance.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT_SIZE = 0x81d0
GUARD_SIZE = 32
SNAPSHOT_SIZE = ROOT_SIZE + 2 * GUARD_SIZE
INPUT_ROW = 16 + SNAPSHOT_SIZE
MAX_ROWS = 16

def fixtures():
    records = []
    for pattern in range(4):
        prefix = bytes((0xd3 + i * 7 + pattern) & 255 for i in range(GUARD_SIZE))
        suffix = bytes((0x29 + i * 13 + pattern) & 255 for i in range(GUARD_SIZE))
        root = bytes(0 if pattern == 0 else 255 if pattern == 1 else
                     0xa5 if pattern == 2 else (i * 73 + 19) & 255
                     for i in range(ROOT_SIZE))
        records.append(bytes(16) + prefix + root + suffix)
    return b''.join(records)

def rows(inputs):
    if not inputs or len(inputs) % INPUT_ROW or len(inputs) > MAX_ROWS * INPUT_ROW:
        raise ValueError('Battle initialization input extent')
    return [inputs[at:at + INPUT_ROW] for at in range(0, len(inputs), INPUT_ROW)]

def expected_snapshots(row):
    if len(row) != INPUT_ROW:
        raise ValueError('Battle initialization input row extent')
    defaults, guarded = row[:16], bytearray(row[16:])
    # A per-byte specification, independent of the native block-store sequence.
    for at in range(ROOT_SIZE):
        index = GUARD_SIZE + at
        if 0x50 <= at < 0x54:
            guarded[index] = 0
        elif 0x60 <= at < 0x8060:
            offset = (at - 0x60) % 0x200
            if offset in (0x16e, 0x16f):
                continue
            if 0x40 <= offset < 0x50:
                value = defaults[offset - 0x40]
            elif (offset in (0x60, 0x61, 0x62, 0x63, 0x66, 0x67) or
                  (0x174 <= offset < 0x1f4 and (offset - 0x174) % 8 < 2)):
                value = 255
            else:
                value = 128 if offset == 0x1ff else 0
            guarded[index] = value
    entities = bytes(guarded)
    for at in range(0x80a4, 0x81ac):
        guarded[GUARD_SIZE + at] = 255 if at < 0x81a4 else 1 if at == 0x81a8 else 0
    return entities, bytes(guarded)

def compare(inputs, original, native):
    records = rows(inputs)
    required = len(records) * 2 * SNAPSHOT_SIZE
    for name, data in (('original', original), ('native', native)):
        if len(data) != required:
            raise ValueError(f'Battle initialization {name} output extent: '
                             f'expected {required}, got {len(data)}')
    for row_number, row in enumerate(records):
        for phase, expected in enumerate(expected_snapshots(row)):
            at = (row_number * 2 + phase) * SNAPSHOT_SIZE
            for name, data in (('original', original), ('native', native)):
                actual = data[at:at + SNAPSHOT_SIZE]
                if actual != expected:
                    first = next(i for i, (a, b) in enumerate(zip(actual, expected)) if a != b)
                    location = (f'prefix+0x{first:x}' if first < GUARD_SIZE else
                                f'root+0x{first - GUARD_SIZE:x}' if first < GUARD_SIZE + ROOT_SIZE else
                                f'suffix+0x{first - GUARD_SIZE - ROOT_SIZE:x}')
                    raise ValueError(f'Battle initialization {name} row {row_number} phase {phase} '
                                     f'{location}: expected {expected[first]:02x}, got {actual[first]:02x}')
    return {'rows': len(records), 'snapshots_per_row': 2, 'snapshot_bytes': SNAPSHOT_SIZE,
            'compared_bytes': required, 'exact': True, 'full_backing_and_canaries': True,
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
