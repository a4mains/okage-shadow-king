#!/usr/bin/env python3
"""Compare effect6's lifetime, ring geometry, deferred transforms and materials.

Original execution uses research RAM; native probes use owned EFD/XIM assets
and synthetic fixtures only. This does not prove raster pixels or scene timing.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from verify_event import write_exact
from verify_map_effect import digest, ELF_SHA
from verify_summon_drawing import fixture, compare_matrices, compare_vertices

HASHES = {
    '6.input.bin': 'c4bc2e0e0900c2b62d91587ea25e9c11f540cbdd943916f2357b45cd5ab4e0bf',
    '6.original.bin': 'f8a1010457b761ce1afc74275fd095e36dbde4306851e03e24b30d8fd0a4d689',
    'geometry-fixture-v1.bin': '1cfba6662844fb8b74b796b09fc8c10666b3703b9d9161ac142ed18c5fda3ad9',
    'geometry-original-v1.bin': '82789fd7a330ba881be38c14a7170a1d3a191be69cf292efd9daa462ec3abaa4',
    'geometry-original-v1.bin.packets.bin': '2de26681586acb2a77e39af3b8203b42c50dc5586ee6f0a5fb87b1a16eccb7e7',
}

def state_fixture():
    return struct.pack('<4I16f', 6,1,0,0x100, -7.73,-0.,-2.5,0,
                       0,0,0,0, 1,1,1,1, 0,0,0,0)

def compare_state(original, native):
    if len(original) != 74*205*4 or len(native) != len(original):
        raise ValueError('Dark effect state extent')
    rows = list(struct.iter_unpack('<205I', original))
    for step, r in enumerate(rows):
        flags = 0x405 if step < 2 else 0x425 if step < 72 else 0x525
        if r[:8] != (step,0,6,flags,4,70,max(1,min(step,71)),1):
            raise ValueError('Dark effect lifetime/RNG observation')
        for i in range(4):
            if r[29+i*44+40:29+i*44+44] != (0xffffffff,0,0xffffffff,0):
                raise ValueError('Dark effect texture/render observation')
    if rows[-1][2:] != rows[-2][2:]:
        raise ValueError('Dark effect terminal state changed')
    if original != native:
        raise ValueError('Native dark effect state differs')
    return len(original)//4

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, required=True)
    p.add_argument('--reference', type=Path, default=Path('local/research/effect6-v1'))
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args();a.output.mkdir(parents=True, exist_ok=False)
    if digest(Path('local/extracted/SCUS_971.29')) != ELF_SHA:
        raise ValueError('Different retail ELF')
    write_exact(a.reference/'6.input.bin', state_fixture())
    write_exact(a.reference/'geometry-fixture-v1.bin', fixture(6))
    for name, sha in HASHES.items():
        if digest(a.reference/name) != sha:
            raise ValueError('Different original corpus: '+name)
    original_runs = [json.loads((a.reference/name).read_text()) for name in
                     ('6.original.run.json','geometry-original-v1.run.json')]
    if any(r['exit_code'] for r in original_runs):
        raise ValueError('Incomplete original execution')
    common = ['local/effect-input/CMNDATA/2D/TES.EFD', 'local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF']
    runs = []
    for mode, source, extra, target in [
        ('map-effect-probe', '6.input.bin', ['72'], 'state.bin'),
        ('dark6-effect-geometry', 'geometry-fixture-v1.bin', [], 'drawing.bin')]:
        command = [str(a.native), mode, *common, str(a.reference/source), *extra, str(a.output/target)]
        subprocess.run(command, check=True)
        runs.append(dict(command=command, exit_code=0, binary_sha256=digest(a.native)))
    words = compare_state((a.reference/'6.original.bin').read_bytes(), (a.output/'state.bin').read_bytes())
    matrices = compare_matrices((a.reference/'geometry-original-v1.bin').read_bytes(), (a.output/'drawing.bin').read_bytes(), 6)
    vertices = compare_vertices((a.reference/'geometry-original-v1.bin.packets.bin').read_bytes(), (a.output/'drawing.bin.vertices.bin').read_bytes(), 6)
    expected = struct.pack('<12I', *[v for i in range(4) for v in (i,2,0)])
    if (a.output/'drawing.bin.materials.bin').read_bytes() != expected:
        raise ValueError('Native subtractive/depth-always material differs')
    report = dict(scope=__doc__.strip(), elf_sha256=ELF_SHA, original_sha256=HASHES,
                  original_runs=original_runs, native_runs=runs, state_words_exact=words,
                  transforms_colors_exact=matrices, vertices_exact=vertices,
                  material='ALPHA0x42, TEST0x30000, ZBUF mask1: subtractive, depth ALWAYS, no writes')
    write_exact(a.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(f'{words} state words, {matrices} transforms/colors and {vertices} vertices exact')

if __name__ == '__main__':
    main()
