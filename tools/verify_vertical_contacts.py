#!/usr/bin/env python3
"""Compare native fresh floor/ceiling contacts with unpatched retail helpers."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact

def stacked_surfaces():
    vertices, polygons = [], []
    for index, (y, normal) in enumerate([(-6, 16384), (-3, -16384), (-2, 16384), (0, -16384)]):
        vertices.extend([(0, y, 0, 1), (8, y, 0, 1), (8, y, 8, 1), (0, y, 8, 1)])
        order = [0, 3, 2, 1, 0] if normal > 0 else [0, 1, 2, 3, 0]
        polygons.append([1 | ((index + 1) << 1), 11 + index, normal & 0xffff, 0, 0] +
                        [index * 4 + i for i in order])
    end = 96 + len(vertices) * 16 + len(polygons) * 20
    data = bytearray(end)
    struct.pack_into('<15i', data, 0, 8, 8, 8, 3, 3, 3, 0, -1, 0, 0, 0, 0, 1, 2, 1)
    for at, pointer in [(60, 64), (64, 68), (68, 72), (72, 80), (76, 80)]:
        struct.pack_into('<I', data, at, pointer)
    struct.pack_into('<HHII', data, 80, len(vertices), len(polygons), 96, 96 + len(vertices) * 16)
    for i, vertex in enumerate(vertices):
        struct.pack_into('<4f', data, 96 + i * 16, *vertex)
    for i, polygon in enumerate(polygons):
        struct.pack_into('<10H', data, 96 + len(vertices) * 16 + i * 20, *polygon)
    return data + bytes([0xc4, 60, 0, 0, 0, 3, 0, 0, 0]) + struct.pack('<I', end)

def fixtures(synthetic):
    rows = []
    for kind in (0, 1):
        ys = (-7., -6., -4., -3., -2.5, -2., -1., 0., 1.) if synthetic else (-8., -4., 0., 3.)
        xs = (-.01, 0., 4., 7.99, 8.01) if synthetic else range(-18, 19, 6)
        zs = (4.,) if synthetic else range(-18, 19, 6)
        for y in ys:
            for x in xs:
                for z in zs:
                    rows.append((x, y-.25, z, 1., x if synthetic else x+.19, y,
                                 z if synthetic else z+.1, 1., 4., 8190, 0, kind))
        for mask, threshold in [(0xffffffff, 8190), (0, 16384), (0, 0), (4, 8190), (8, 8190)]:
            rows.append((4., -2.5, 4., 1., 4., -2.5, 4., 1., 4., threshold, mask, kind))
    return b''.join(struct.pack('<9f3I', *row) for row in rows)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--reference', type=Path, help='Completed reference directory for these fixtures')
    parser.add_argument('--output', type=Path, default=Path('local/vertical-validation'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    elf = Path('local/extracted/SCUS_971.29')
    oracle = Path('build/play-research/okage-oracle/okage-pose-oracle')
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    if digest(elf) != '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5':
        raise RuntimeError('Unexpected retail ELF')
    report = {'scope': 'Fresh floor/ceiling queries, correction, surface ID, normal XYZ and selected polygon metadata',
              'functions': ['0x18a0e0', '0x18a640'],
              'excluded': ['cached/resumed queries', 'undefined normal W from retail stack',
                           'stale metadata when hit=false', 'wall/dynamic collision', 'app movement'],
              'preconditions': 'Synthetic object record, owned or synthetic map, unpatched helpers; no captured RAM or OS calls',
              'elf_sha256': digest(elf), 'native_sha256': digest(args.native), 'cases': []}
    if not args.reference:
        report['oracle_sha256'] = digest(oracle)
    for name in ('home1c', 'stacked'):
        asset = Path('local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF/a.xcl')
        if name == 'stacked':
            asset = args.output / 'stacked.xcl'
            write_exact(asset, stacked_surfaces())
        fixture = args.output / (name + '.queries.bin')
        data = fixtures(name == 'stacked')
        write_exact(fixture, data)
        reference = (args.reference or args.output) / (name + '.retail.bin')
        if not args.reference:
            subprocess.run([str(oracle), 'xcl-vertical', str(elf), str(asset), str(fixture), str(reference)], check=True)
        native = args.output / (name + '.native.bin')
        subprocess.run([str(args.native), 'xcl-vertical', str(asset), str(fixture), str(native)], check=True)
        expected = reference.read_bytes()
        if len(expected) != len(data) // 48 * 64 or expected != native.read_bytes():
            raise RuntimeError(name + ': original/native vertical contact mismatch')
        results = list(struct.iter_unpack('<16I', expected))
        hits = [sum(row[0] for row in results[:len(results)//2]),
                sum(row[0] for row in results[len(results)//2:])]
        if name == 'stacked' and not all(hits):
            raise RuntimeError('Synthetic fixture does not exercise both contact directions')
        report['cases'].append({'name': name, 'count': len(results), 'exact': True,
                                'floor_hits': hits[0], 'ceiling_hits': hits[1],
                                'hashes': {str(p): digest(p) for p in [asset, fixture, reference, native]}})
    write_exact(args.output / 'report.json', (json.dumps(report, indent=2) + '\n').encode())
    print('Original/native floor and ceiling contacts match exactly')

if __name__ == '__main__':
    main()
