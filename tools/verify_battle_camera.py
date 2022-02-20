#!/usr/bin/env python3
"""Reproduce and compare the bounded original/native battle-camera motion probe.

This compares captured bytes, not an independent motion algorithm, pixels,
projection submission, channel roundtrips, or the complete camera callback.
No original or native executable is launched.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

ELF_HASH = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
FIXTURE_HASH = 'f4a91dab0fd0b3cfe283a198fcbba19bb0ef39eada2887e87f12cb3de0887b77'
ROW_SIZE = 448
SNAPSHOT_SIZE = 224

def fixtures(elf):
    if hashlib.sha256(elf).hexdigest() != ELF_HASH:
        raise ValueError('Battle camera generator requires pinned USA ELF hash')
    phoff = struct.unpack_from('<I', elf, 28)[0]
    phsize, phcount = struct.unpack_from('<HH', elf, 42)

    def read(va, length):
        for i in range(phcount):
            kind, offset, address, _, size, _, _, _ = struct.unpack_from('<8I', elf, phoff + i * phsize)
            if kind == 1 and address <= va and va + length <= address + size:
                return elf[offset + va - address:offset + va - address + length]
        raise ValueError('Battle camera ELF virtual extent')

    result = bytearray()
    for entry, timestep, ticks in ((228, 1., 610), (229, 1., 310),
                                    (228, 2., 310), (228, .5, 1210)):
        pointers = struct.unpack('<3I', read(0x235ea0 + entry * 12, 12))
        mask = sum(1 << i for i, pointer in enumerate(pointers) if pointer)
        result.extend(struct.pack('<f3I', timestep, ticks, mask, 0))
        for pointer in pointers:
            result.extend(read(pointer, 144) if pointer else bytes(144))
    for mode, timestep, duration, mask, ticks in (
            (0, 1., 2., 1, 6), (1, 1., 2., 1, 6), (1, .5, 2., 7, 20),
            (1, 2., 3., 3, 10), (1, 1.5, 7., 5, 15)):
        result.extend(struct.pack('<f3I', timestep, ticks, mask, 0))
        for i in range(3):
            preset = bytearray(144)
            if mask & (1 << i):
                for at, first, last in (
                        (0, (4. * i, 2., 0., 0.), (4. * i + 3., 1., 2., 0.)),
                        (48, (0., -2., 10., 0.), (3., 0., 7., 0.))):
                    struct.pack_into('<If2I8f', preset, at, mode, duration, 0, 0, *first, *last)
                struct.pack_into('<f', preset, 100, duration)
                struct.pack_into('<f', preset, 112, 800.)
            result.extend(preset)
    data = bytes(result)
    rows(data)
    if hashlib.sha256(data).hexdigest() != FIXTURE_HASH:
        raise ValueError('Battle camera generated fixture identity mismatch')
    return data

def rows(inputs):
    if not inputs or len(inputs) % ROW_SIZE or len(inputs) > 16 * ROW_SIZE:
        raise ValueError('Battle camera input extent: expected 1..16 whole 448-byte rows')
    headers = []
    for number, at in enumerate(range(0, len(inputs), ROW_SIZE)):
        timestep, ticks, mask, reserved = struct.unpack_from('<f3I', inputs, at)
        if not math.isfinite(timestep) or timestep <= 0 or ticks > 2000 or not mask & 1 or mask > 7 or reserved:
            raise ValueError(f'Battle camera row {number} header bounds')
        for i in range(3):
            if not mask & (1 << i):
                continue
            p = at + 16 + i * 144
            position, target = (struct.unpack_from('<I', inputs, p + off)[0] for off in (0, 48))
            if position > 1 or target > 1:
                raise ValueError(f'Battle camera row {number} preset {i} mode')
            durations = [struct.unpack_from('<f', inputs, p + off)[0] for off in (4, 52, 100, 112)]
            if any(not math.isfinite(f) or f < 0 for f in durations):
                raise ValueError(f'Battle camera row {number} preset {i} parameter bounds')
            if (position == 1 and durations[0] == 0) or (target == 1 and durations[1] == 0):
                raise ValueError(f'Battle camera row {number} preset {i} linear duration')
            points = struct.unpack_from('<8f', inputs, p + 16) + struct.unpack_from('<8f', inputs, p + 64)
            if any(not math.isfinite(f) or abs(f) > 1e6 for f in points):
                raise ValueError(f'Battle camera row {number} preset {i} point bounds')
            if points[7] != 0:
                raise ValueError(f'Battle camera row {number} preset {i} position endpoint W')
        headers.append({'timestep': timestep, 'ticks': ticks, 'presence_mask': mask})
    return headers

def compare(inputs, original, native):
    headers = rows(inputs)
    snapshots = sum(row['ticks'] + 1 for row in headers)
    size = snapshots * SNAPSHOT_SIZE
    for name, output in (('original', original), ('native', native)):
        if len(output) != size:
            raise ValueError(f'Battle camera {name} output extent: expected {size}, got {len(output)}')
    mismatch = None
    if original != native:
        byte = next(i for i, (a, b) in enumerate(zip(original, native)) if a != b)
        snapshot, within = divmod(byte, SNAPSHOT_SIZE)
        tick = snapshot
        for number, row in enumerate(headers):
            if tick <= row['ticks']:
                break
            tick -= row['ticks'] + 1
        word = within // 4
        offset = snapshot * SNAPSHOT_SIZE + word * 4
        mismatch = {'row': number, 'tick': tick, 'initial_snapshot': tick == 0,
                    'snapshot': snapshot, 'word': word, 'byte_offset': byte,
                    'location': 'sequence_phase' if word == 55 else f'body+0x{0x10 + word * 4:x}',
                    'original_word': f'0x{struct.unpack_from("<I", original, offset)[0]:08x}',
                    'native_word': f'0x{struct.unpack_from("<I", native, offset)[0]:08x}'}
    return {'rows': len(headers), 'snapshots': snapshots, 'words_per_snapshot': 56,
            'compared_bytes': size, 'exact': mismatch is None, 'first_difference': mismatch,
            'scope': __doc__.strip(), 'independent_algorithm_check': False,
            'sha256': {name: hashlib.sha256(data).hexdigest() for name, data in
                       (('input', inputs), ('original', original), ('native', native))}}

def write_fresh(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as output:
        output.write(data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    generate = commands.add_parser('generate')
    generate.add_argument('elf', type=Path)
    generate.add_argument('output', type=Path)
    verify = commands.add_parser('verify')
    for name in ('input', 'original', 'native'):
        verify.add_argument(name, type=Path)
    verify.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.command == 'generate':
        data = fixtures(args.elf.read_bytes())
        write_fresh(args.output, data)
        print(f'{len(rows(data))} rows; sha256 {hashlib.sha256(data).hexdigest()}')
        return
    result = compare(args.input.read_bytes(), args.original.read_bytes(), args.native.read_bytes())
    result['paths'] = {name: str(getattr(args, name)) for name in ('input', 'original', 'native')}
    encoded = json.dumps(result, indent=2) + '\n'
    if args.report:
        write_fresh(args.report, encoded.encode())
    print(encoded, end='')
    if not result['exact']:
        raise SystemExit(1)

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from error
