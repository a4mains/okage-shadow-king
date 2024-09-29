#!/usr/bin/env python3
"""Compare ordinary-mode retail block selection/sorting to native bounds.

The original command starts inside the unchanged ordinary callback at 0x18606c,
stops before its branch at 0x1862e8, then runs the insertion sort at
0x1862fc..0x1863b8. Explicit synthetic bounds and already-composed poses are
used; this is not model setup, pose generation, packet, or raster evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time
from verify_event import write_exact
from verify_map_effect import ELF_SHA
from verify_shadow_visibility import compare, fixtures as shadow_fixtures

SOURCE_ASM_SHA256 = 'd335385b9e73b06f28184d9dd02adbe0ee150571d2b32142fc75f2de89eff480'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def fixtures():
    _, cases = shadow_fixtures()
    cases = [dict(case, culling=1) for case in cases]
    data = bytearray()
    for case in cases:
        data += struct.pack('<4I15f', case['id'], 1, len(case['blocks']),
                            len(case['poses']), *case['screen'])
        for pose in case['poses']:
            data += struct.pack('<16f', *pose)
        for block in case['blocks']:
            data += struct.pack('<I', len(block))
            for bound in block:
                data += struct.pack('<5fI', *bound)
    return bytes(data), cases

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True,
                        help='Built asset tool exposing shadow-visibility')
    parser.add_argument('--oracle', type=Path,
                        default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    parser.add_argument('--reference', type=Path,
                        default=Path('local/research/ordinary-visibility-v1'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    args.reference.mkdir(parents=True, exist_ok=True)
    data, cases = fixtures()
    fixture = args.reference / 'ordinary-visibility-fixture-v1.bin'
    write_exact(fixture, data)
    elf = Path('local/extracted/SCUS_971.29')
    if digest(elf) != ELF_SHA:
        raise ValueError('Different owned ELF')
    source_asm = Path('local/research/00185fb0.asm')
    if digest(source_asm) != SOURCE_ASM_SHA256:
        raise ValueError('Ordinary selection source export changed')
    original = args.reference / 'ordinary-visibility-original-v1.bin'
    manifest = original.with_suffix('.run.json')
    if not original.exists():
        if manifest.exists():
            raise ValueError('Retained original failure; investigate before retry')
        command = [str(args.oracle), 'ordinary-visibility', str(elf), str(fixture), str(original)]
        start = time.monotonic()
        with original.with_suffix('.log').open('x') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        run = dict(command=command, exit_code=result.returncode,
                   elapsed_seconds=time.monotonic() - start,
                   binary_sha256=digest(args.oracle), elf_sha256=ELF_SHA,
                   input_sha256=digest(fixture))
        if not result.returncode:
            run['output_sha256'] = digest(original)
        write_exact(manifest, (json.dumps(run, indent=2) + '\n').encode())
        if result.returncode:
            raise RuntimeError('Original ordinary visibility failed')
    run = json.loads(manifest.read_text())
    if (run['exit_code'] or run['elf_sha256'] != ELF_SHA or
            run['input_sha256'] != digest(fixture) or
            run['output_sha256'] != digest(original)):
        raise ValueError('Changed or incomplete original ordinary visibility')
    native = args.output / 'native.bin'
    command = [str(args.native), 'shadow-visibility', str(fixture), str(native)]
    subprocess.run(command, check=True)
    metrics = compare(original.read_bytes(), native.read_bytes(), cases)
    report = dict(schema='ordinary-model-visibility-comparison-v1',
                  scope=__doc__, original_run=run, native_command=command,
                  native_sha256=digest(args.native), fixture_sha256=digest(fixture),
                  source_asm_path=str(source_asm), source_asm_sha256=SOURCE_ASM_SHA256,
                  **metrics)
    write_exact(args.output / 'report.json', (json.dumps(report, indent=2) + '\n').encode())
    print(json.dumps({key: value for key, value in report.items() if key.endswith('_exact')}))

if __name__ == '__main__':
    main()
