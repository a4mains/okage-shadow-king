#!/usr/bin/env python3
"""Bounded original EE/VU1 ordinary normal, light-matrix and RGB comparison.

Finite synthetic cases cover cached, overwrite, accumulation and direct paths.
This is not complete model submission, GS raster or scene-fidelity evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
from prepare_assets import ELF_HASH
from verify_event import write_exact

def fixtures():
    rng = random.Random(0x17d510)
    identity = [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.]
    records = []
    for case in range(64):
        matrix = identity.copy()
        view = identity.copy()
        if case % 4:
            matrix[:12] = [rng.uniform(-2, 2) if i % 4 != 3 else 0. for i in range(12)]
            matrix[12:15] = [12., -23., 34.]  # Must not affect normal transform.
            view = [.8, 0., -.6, 0., 0., 1., 0., 0., .6, 0., .8, 0., 7., 8., 9., 1.]
        normals = [0., 0., 1., 0., 1., 0., 0., 0., 0., 1., 0., 0.]
        if case:
            normals = [rng.uniform(-4, 4) if i % 4 != 3 else .125 * (i // 4) for i in range(12)]
        prior = [rng.uniform(-.25, .25) for _ in range(12)]
        directions = [0., 0., -1., 0., -1., 0., 0., 0., 0., -1., 0., 0.]
        if case % 3:
            directions = [rng.uniform(-2, 2) if i % 4 != 3 else 19. for i in range(12)]
        colors = [1., 1., 1., 0., 0., 0., 0., 0., 0., 0., 0., 0., .5, .5, .5, 1.]
        if case > 1:
            colors = [rng.uniform(-.3, 1.5) for _ in range(16)]
        source = [255., 128., 76., 128., 3.5, 190.25, 255., 77., 0., 100., 212., 128.]
        payload = matrix + normals + prior + directions + view + colors + source
        assert len(payload) == 96
        for mode in range(4):
            records.append(struct.pack('<4I96f', len(records), mode, 0, 0, *payload))
    return b''.join(records)

def compare(original, native):
    if len(original) != len(native) or not original or len(original) % 272:
        raise ValueError('Lighting output extent mismatch')
    differences = []
    for offset in range(0, len(original), 4):
        if original[offset:offset+4] != native[offset:offset+4]:
            differences.append(dict(record=offset//272, word=(offset % 272)//4,
                original=struct.unpack_from('<I', original, offset)[0],
                native=struct.unpack_from('<I', native, offset)[0]))
    return differences

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--original', type=Path, help='Reuse a retained original output for these exact fixtures')
    parser.add_argument('--build', action='append', default=[])
    parser.add_argument('--reference-only', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    elf = Path('local/extracted/SCUS_971.29')
    if sha(elf) != ELF_HASH:
        raise ValueError('Wrong ELF')
    data = fixtures()
    inputs = args.output/'cases.bin'
    write_exact(inputs, data)
    commands = []
    artifacts = [elf, inputs]
    if args.original:
        # The paired fixture file is mandatory: an unrelated reference must fail.
        if (args.original.parent/'cases.bin').read_bytes() != data:
            raise ValueError('Retained original fixture mismatch')
        original = args.original
    else:
        oracle = Path('build/play-research/okage-oracle/okage-pose-oracle')
        original = args.output/'original.bin'
        command = [str(oracle), 'ordinary-lighting', str(elf), str(inputs), str(original)]
        subprocess.run(command, check=True)
        commands.append(command)
        artifacts.append(oracle)
    expected = original.read_bytes()
    if len(expected) != len(data)//400*272:
        raise ValueError('Original output extent')
    artifacts.append(original)
    results = {}
    if not args.reference_only:
        for build in args.build or ['native-release']:
            binary = Path('build')/build/'okage-assets'
            output = args.output/(build+'.bin')
            command = [str(binary), 'ordinary-lighting', str(inputs), str(output)]
            subprocess.run(command, check=True)
            commands.append(command)
            diffs = compare(expected, output.read_bytes())
            results[build] = dict(differing_words=len(diffs), differences=diffs[:100])
            artifacts.extend([binary, output])
    report = dict(scope=__doc__, elf_sha256=ELF_HASH, cases=len(data)//400,
                  native_compared=bool(results), results=results, commands=commands,
                  sha256={str(p): sha(p) for p in artifacts})
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    if any(r['differing_words'] for r in results.values()):
        raise SystemExit('Ordinary lighting mismatch; see retained report')
    print('Ordinary lighting cases:', len(data)//400, 'native builds:', len(results))

if __name__ == '__main__':
    main()
