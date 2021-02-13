#!/usr/bin/env python3
"""Compare native capsule-wall contacts with the unpatched retail helper."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
from verify_event import write_exact

def wall_asset():
    vertices = [(4, -8, 0, 1), (4, -8, 8, 1), (4, 0, 8, 1), (4, 0, 0, 1)]
    end = 180
    data = bytearray(end)
    struct.pack_into('<15i', data, 0, 8, 8, 8, 3, 3, 3, 0, -1, 0, 0, 0, 0, 1, 2, 1)
    for at, pointer in [(60, 64), (64, 68), (68, 72), (72, 80), (76, 80)]:
        struct.pack_into('<I', data, at, pointer)
    struct.pack_into('<HHII', data, 80, 4, 1, 96, 160)
    for i, vertex in enumerate(vertices):
        struct.pack_into('<4f', data, 96 + i * 16, *vertex)
    struct.pack_into('<10H', data, 160, 3, 11, 0, 0xc000, 0, 0, 1, 2, 3, 0)
    return data + bytes([0xc4, 60, 0, 0, 0, 3, 0, 0, 0]) + struct.pack('<I', end)

def fixtures(synthetic):
    rows = []
    def add(previous, desired, first=(0, -2.5, 0, 1), second=(0, -5, 0, 1), radius=1.9, threshold=8190, mask=0):
        rows.append((*previous, 1, *desired, 1, *first, *second, radius, threshold, mask))
    if synthetic:
        for x in (1.9, 2.1, 2.5, 3.9, 4., 4.1):
            for z in (-2., -1.5, -.1, 0., .1, 4., 7.9, 8., 8.1, 9.5, 10.):
                for y in (-6., 0., 4., 6.):
                    add((x-.55, y, z), (x, y, z))
        for first, second in [((0, -2.5, 0, 1), (1, -5, 0, 1)),
                              ((0, -2.5, 0, 1), (0, -5, 1, 1))]:
            for z in (-1., 0., 4., 8., 9.):
                add((2, 0, z), (2.5, 0, z), first, second)
        for mask, threshold in [(0, 0), (0, 16384), (2, 8190), (0xffffffff, 8190)]:
            add((2, 0, 4), (2.5, 0, 4), mask=mask, threshold=threshold)
        add((2.5, 0, 4), (2.5, 0, 4))
        add((3, 0, 4), (2.5, 0, 4))
    else:
        rng = random.Random(0x18bb10)
        for _ in range(400):
            x, z = rng.uniform(-20, 20), rng.uniform(-20, 20)
            dx, dz = rng.choice([(-.55, 0), (.55, 0), (0, -.55), (0, .55)])
            add((x, 0, z), (x+dx, 0, z+dz))
        actor = Path('local/reference-kitchen-movement-180-v1.actor.bin').read_bytes()
        if len(actor) != 180*920*4:
            raise RuntimeError('Unexpected movement trace extent')
        for at in range(0, len(actor), 920*4):
            x, y, z = struct.unpack_from('<3f', actor, at+12*4)
            for dx, dz in [(-.55, 0), (.55, 0), (0, -.55), (0, .55)]:
                add((x, y, z), (x+dx, y, z+dz))
    return b''.join(struct.pack('<17f2I', *row) for row in rows)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--output', type=Path, default=Path('local/wall-validation'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    elf = Path('local/extracted/SCUS_971.29')
    oracle = Path('build/play-research/okage-oracle/okage-pose-oracle')
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    if digest(elf) != '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5':
        raise RuntimeError('Unexpected retail ELF')
    report = {'scope': "Fresh capsule-wall contacts: hit, flags, material, correction XYZW and normal XYZ",
              'function': '0x18bb10', 'elf_sha256': digest(elf), 'native_sha256': digest(args.native),
              'excluded': ['undefined normal W', 'stale failure metadata', 'cached/resumed queries',
                           'dynamic actor contacts', 'collision composition', 'app movement'],
              'preconditions': 'Synthetic record and capsule; owned/synthetic XCL; original helpers without captured RAM or OS',
              'cases': []}
    if not args.reference:
        report['oracle_sha256'] = digest(oracle)
    for name in ('wall', 'home1c'):
        asset = Path('local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF/a.xcl')
        if name == 'wall':
            asset = args.output / 'wall.xcl'
            write_exact(asset, wall_asset())
        fixture = args.output / (name+'.queries.bin')
        data = fixtures(name == 'wall')
        write_exact(fixture, data)
        reference = (args.reference or args.output) / (name+'.retail.bin')
        if args.reference and (args.reference / fixture.name).read_bytes() != data:
            raise RuntimeError(name+': reference queries differ')
        if not args.reference:
            subprocess.run([str(oracle), 'xcl-wall', str(elf), str(asset), str(fixture), str(reference)], check=True)
        native = args.output / (name+'.native.bin')
        subprocess.run([str(args.native), 'xcl-wall', str(asset), str(fixture), str(native)], check=True)
        expected, actual = reference.read_bytes(), native.read_bytes()
        if len(expected) != len(data)//76*40 or len(actual) != len(expected):
            raise RuntimeError(name+': unexpected output extent')
        differences = [i//40 for i in range(0,len(expected),40) if expected[i:i+40] != actual[i:i+40]]
        if differences:
            raise RuntimeError(f'{name}: {len(differences)} mismatched rows; first {differences[:20]}')
        results = list(struct.iter_unpack('<10I', expected))
        hits = sum(r[0] for r in results)
        if not 0 < hits < len(results):
            raise RuntimeError(name+': expected both hits and rejections')
        report['cases'].append({'name': name, 'count': len(results), 'hits': hits, 'exact': True,
                                'hashes': {str(p): digest(p) for p in (asset, fixture, reference, native)}})
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print('Original/native capsule-wall contacts match exactly')

if __name__ == '__main__':
    main()
