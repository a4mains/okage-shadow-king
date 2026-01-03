#!/usr/bin/env python3
"""Compare native XMP transforms with a bounded retail EE instruction fragment.

Requires the existing pinned research oracle. Raw fixtures/output remain ignored.
This does not execute scene creation, effect handlers, collision or event logic.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def fixture():
    definitions = [0, 0x80, 0x100, 0x100080]
    scales = [(1, 1, 1), (2, 2, 1), (2, 1, 0), (2, 1, 1),
              (2, 2, 0), (-1, -1, 1), (0.1, 0.1, 0.1)]
    angles = [(0, 0, 0), (90, 180, -90), (359.99, -720, 450.25)]
    count = len(definitions) * len(scales) * len(angles) * 2
    data = bytearray(64 + count * 80 + len(definitions) * 148)
    struct.pack_into('<II', data, 8, 64, count)
    struct.pack_into('<II', data, 40, 64 + count * 80, len(definitions))
    for i, flags in enumerate(definitions):
        at = 64 + count * 80 + i * 148
        data[at:at+9] = b'test.xmd\0'
        struct.pack_into('<I', data, at + 28, flags)
    i = 0
    for d in range(len(definitions)):
        for scale in scales:
            for rotation in angles:
                for flags in [0, 0x5b100080]:
                    at = 64 + i * 80
                    struct.pack_into('<IIII', data, at, 1, 0xffffffff, d, flags)
                    struct.pack_into('<12f', data, at + 16,
                                     12.25, -3.125, -0.0, 17,
                                     *rotation, -0.0, *scale, 0.25)
                    i += 1
    return bytes(data)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--oracle', type=Path, default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--elf', type=Path, default=Path('local/extracted/SCUS_971.29'))
    p.add_argument('--root', type=Path, default=Path('local/xpf-decoded'))
    p.add_argument('--output', type=Path, default=Path('local/map-validation'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    synthetic = args.output / 'synthetic.xmp'
    data = fixture()
    if synthetic.exists() and synthetic.read_bytes() != data:
        raise ValueError('Refusing changed synthetic fixture')
    synthetic.write_bytes(data)
    records = []
    maps = sorted(args.root.rglob('*.xmp'))
    if not maps:
        raise ValueError('No XMP corpus found')
    for i, path in enumerate([synthetic] + maps):
        native = args.output / ('%04d.native.bin' % i)
        retail = args.output / ('%04d.retail.bin' % i)
        subprocess.run([str(args.native), 'map-transforms', str(path), str(native)], check=True)
        subprocess.run([str(args.oracle), 'map-transforms', str(args.elf), str(path), str(retail)], check=True)
        expected_count = struct.unpack_from('<I', path.read_bytes(), 12)[0]
        if native.read_bytes() != retail.read_bytes() or native.stat().st_size != expected_count * 52:
            raise ValueError('Transform mismatch or extent error: ' + str(path))
        records.append({'file': str(path), 'sha256': sha(path), 'placements': expected_count,
                        'transform_sha256': sha(native)})
        if i % 25 == 0:
            print('Verified %d/%d XMP fixtures' % (i + 1, len(maps) + 1), flush=True)
    report = {'scope': 'Inline XMP parsing and 12 transform floats plus merged flags; '
              'bit-exact comparison against relocated retail 0x1bbd28..0x1bbe3f fragment. '
              'Not model loading, effects, scene rendering, events or playability.',
              'elf_sha256': sha(args.elf), 'native_sha256': sha(args.native),
              'oracle_sha256': sha(args.oracle), 'archived_maps': len(maps),
              'archived_placements': sum(x['placements'] for x in records[1:]),
              'synthetic_placements': records[0]['placements'], 'exact_match': True,
              'records': records}
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'records'}, indent=2))

if __name__ == '__main__':
    main()
