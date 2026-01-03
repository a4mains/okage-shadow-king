#!/usr/bin/env python3
"""Compare native subdivided map movement with original wall/ceiling/floor composition."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_wall_contacts import fixtures as wall_fixtures, wall_asset
from verify_vertical_contacts import stacked_surfaces

def room_asset():
    data = bytearray(wall_asset()[:96]) + bytearray(8*16+2*20)
    struct.pack_into('<i', data, 28, 0)  # Y origin, two occupied cells.
    struct.pack_into('<i', data, 40, 1)
    struct.pack_into('<HHII', data, 80, 8, 2, 96, 224)
    vertices = [(4,0,0,1),(4,0,8,1),(4,8,8,1),(4,8,0,1),
                (0,6,0,1),(8,6,0,1),(8,6,8,1),(0,6,8,1)]
    for i, v in enumerate(vertices):
        struct.pack_into('<4f', data, 96+i*16, *v)
    struct.pack_into('<10H', data, 224, 3,11,0,0xc000,0,0,1,2,3,0)
    struct.pack_into('<10H', data, 244, 1,13,0xc000,0,0,4,5,6,7,4)
    return data + bytes([0xc4,60,0,0,0,3,0,0,0]) + struct.pack('<I', len(data))

def fixtures(synthetic):
    queries = []
    for row in struct.iter_unpack('<17f2I', wall_fixtures(synthetic)):
        queries.append(struct.pack('<17f2I6f3I', *row, 0, 0, 0, 1, 4, .5, 2, 8, 0x72))
    for end in [(2.1, 0, 4), (2.5, 0, 4), (4, 0, 4), (7, 0, 4),
                (2, -3, 4), (2, 3, 4), (7, -3, 8), (2, 0, 4)]:
        for iterations in (0, 1, 2, 3):
            for point_flags, capsule_flags in [(8, 0x72), (0, 0x32), (8, 2), (0x10, 0x3a)]:
                queries.append(struct.pack('<17f2I6f3I', 2, 0, 4, 1, *end, 1,
                                           0, -2.5, 0, 1, 0, -5, 0, 1, 1.9, 8190, 0,
                                           0, 0, 0, 1, 4, .5, iterations, point_flags, capsule_flags))
    return b''.join(queries)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--output', type=Path, default=Path('local/map-contact-validation'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    elf = Path('local/extracted/SCUS_971.29')
    oracle = Path('build/play-research/okage-oracle/okage-pose-oracle')
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    if digest(elf) != '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5':
        raise RuntimeError('Unexpected retail ELF')
    report = {'scope': 'Point/capsule map contacts with movement subdivision, ordered correction, first material IDs and final normals',
              'functions': ['0x187270', '0x187498'], 'elf_sha256': digest(elf), 'native_sha256': digest(args.native),
              'excluded': ['other shape types', 'point wall contacts', 'cached/resumed queries',
                           'undefined normal W', 'dynamic actor contacts', 'outer registry resolution', 'app movement'],
              'cases': []}
    if not args.reference:
        report['oracle_sha256'] = digest(oracle)
    for name in ('wall', 'stacked', 'home1c', 'room'):
        asset = Path('local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF/a.xcl')
        if name != 'home1c':
            asset = args.output / (name+'.xcl')
            write_exact(asset, {'wall': wall_asset, 'stacked': stacked_surfaces, 'room': room_asset}[name]())
        fixture = args.output / (name+'.queries.bin')
        data = fixtures(name != 'home1c')
        if name == 'room':
            data = b''.join(struct.pack('<17f2I6f3I', 2,6,4,1, 7,6,4,1,
                                        0,-2.5,0,1, 0,-5,0,1, 1.9,8190,0,
                                        0,0,0,1,4,.5, iterations,8,0x72) for iterations in (0,2))
        write_exact(fixture, data)
        reference = (args.reference or args.output) / (name+'.retail.bin')
        if args.reference and (args.reference/fixture.name).read_bytes() != data:
            raise RuntimeError('Different reference queries')
        if not args.reference:
            subprocess.run([str(oracle), 'xcl-move', str(elf), str(asset), str(fixture), str(reference)], check=True)
        native = args.output / (name+'.native.bin')
        subprocess.run([str(args.native), 'xcl-move', str(asset), str(fixture), str(native)], check=True)
        expected, actual = reference.read_bytes(), native.read_bytes()
        if len(expected) != len(data)//112*72 or len(actual) != len(expected):
            raise RuntimeError(name+': unexpected result size')
        diffs = [i//72 for i in range(0,len(expected),72) if expected[i:i+72] != actual[i:i+72]]
        if diffs:
            raise RuntimeError(f'{name}: {len(diffs)} mismatched rows; first {diffs[:20]}')
        rows = list(struct.iter_unpack('<18I', expected))
        counts = {str(flags): sum(r[1] == flags for r in rows) for flags in sorted(set(r[1] for r in rows))}
        report['cases'].append({'name': name, 'count': len(rows), 'exact': True, 'flags': counts,
                                'hashes': {str(p): digest(p) for p in [asset,fixture,reference,native]}})
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print('Original/native subdivided map contacts match exactly')

if __name__ == '__main__':
    main()
