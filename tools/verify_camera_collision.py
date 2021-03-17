#!/usr/bin/env python3
"""Compare camera sphere movement and obstruction rays with original execution."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_map_contacts import room_asset

def fixtures(outdoor=False):
    rows = []
    moves = [((2, 4, 4), (5, 4, 4)), ((5, 4, 4), (2, 4, 4)),
             ((2, 4, 4), (2, 4, 4)), ((2, 4, 4), (3, 4, 4)),
             ((2, 4, -1), (3, 4, -1)), ((2, -1, -1), (3, -1, -1)),
             ((1, 3, 1), (1, 8, 1)), ((1, 8, 1), (1, 3, 1)),
             ((-10, 4, 4), (20, 4, 4)), ((1, 4, 1), (7, 4, 7))]
    if outdoor:
        moves = []
        for x, y, z in ((283.25, -37.25, 689.66), (280, -50, 730),
                        (230, -20, 700), (300, -70, 690)):
            for dx, dy, dz in ((0, 30, 0), (0, -40, 0), (60, 0, 0),
                               (-60, 0, 0), (0, 0, 60), (0, 0, -60), (20, 10, 20)):
                moves.append(((x, y, z), (x+dx, y+dy, z+dz)))
    for first, second in moves:
        for mode in (0, 1, 2):
            for mask in (0, 4):
                radius = 4.5 if mode == 2 else 1.9
                rows.append(struct.pack('<8IIfII', *struct.unpack('<8I', struct.pack('<8f', *first, 1, *second, 1)),
                                        mode, radius, mask, 8190))
    return b''.join(rows)

def compare(expected, actual, count):
    if len(expected) != count*88 or len(actual) != len(expected):
        raise ValueError('Camera collision output extent')
    failures = []
    for i, (a, b) in enumerate(zip(struct.iter_unpack('<22I', expected), struct.iter_unpack('<22I', actual))):
        words = [j for j, (x, y) in enumerate(zip(a, b)) if x != y]
        if words:
            failures.append((i, words))
    if failures:
        raise ValueError(f'{len(failures)} camera collision cases differ: {failures[:12]}')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    oracle = Path('build/play-research/okage-oracle/okage-pose-oracle')
    elf = Path('local/extracted/SCUS_971.29')
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    if digest(elf) != '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5':
        raise ValueError('Unexpected ELF')
    report = {'scope': "Original sphere-wall, segment obstruction and camera sphere map resolution",
              'elf_sha256': digest(elf), 'native_sha256': digest(args.native), 'cases': [], 'commands': [],
              'limits': ['Bounded synthetic inputs, not a complete outdoor camera or scene.',
                         'Normal W is undefined in the original and excluded.',
                         'Original query services are unpatched; no reference RAM is required.']}
    for name in ('room', 'outdoor'):
        asset = Path('local/xpf-decoded/FLDDATA/MAP/FIELD/FIELD_1D.XPF/a.xcl')
        if name == 'room':
            asset = args.output / 'room.xcl'
            write_exact(asset, room_asset())
        inputs = args.output / (name+'.inputs.bin')
        data = fixtures(name == 'outdoor')
        write_exact(inputs, data)
        reference = (args.reference or args.output) / (name+'.original.bin')
        if args.reference:
            if (args.reference / inputs.name).read_bytes() != data:
                raise ValueError('Original camera collision inputs differ')
        else:
            command = [str(oracle), 'xcl-camera', str(elf), str(asset), str(inputs), str(reference)]
            report['commands'].append(command)
            subprocess.run(command, check=True)
        native = args.output / (name+'.native.bin')
        command = [str(args.native), 'xcl-camera', str(asset), str(inputs), str(native)]
        report['commands'].append(command)
        subprocess.run(command, check=True)
        expected, actual = reference.read_bytes(), native.read_bytes()
        compare(expected, actual, len(data)//48)
        records = list(struct.iter_unpack('<22I', expected))
        report['cases'].append({'name': name, 'count': len(records), 'exact': True,
                                'hits': sum(bool(r[0]) for r in records),
                                'sha256': {str(p): digest(p) for p in (asset, inputs, reference, native)}})
    if not args.reference:
        report['oracle_sha256'] = digest(oracle)
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print('Camera collision comparisons pass:', args.output)

if __name__ == '__main__':
    main()
