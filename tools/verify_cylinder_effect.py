#!/usr/bin/env python3
"""Compare native effect26 state, shared UV animation, geometry and materials.

Original probes execute the positioned event service and bounded draw blocks.
This does not establish scene timing, RNG startup, raster or audio equivalence.
Research RAM is never an input to the native application.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from verify_event import write_exact
from verify_map_effect import digest, ELF_SHA
from verify_map_geometry import compare_vertices

HASHES = {
    '26.input.bin': 'df87b64aced6fea436774cc473c545b96d5a963a792763e4864bc1297d3cd98e',
    '26.original.bin': 'd488ce8fe9be1fac7c4b0e18e9e53c16aedc6524aa5693f94eae4b644fc98ce6',
    '26.original.bin.uv.bin': '5206534e1dac74ae0f3ac8a69f0993ea8ad528fe18632f160f274e8d7e9771ea',
    'geometry-fixture-v1.bin': '0f1f5b9b3e3b45bc8684ad33d308d3224e67462d242c3b32fdb3a8ebd3d43018',
    'geometry-original-v1.bin': '251f324cd18e7abc7a23a118c0f5b2e4f567a10b95cfdbdb340f75663b0040b3',
    'geometry-original-v1.bin.packets.bin': 'bd739f88da5f43fb4d6168490e1cff7ad5deb609275b7f0326d256bf84787e99',
}

def state_fixture():
    return struct.pack('<4I16f', 26,1,0,0x100, -7.73,-0.,-2.95,0,
                       0,0,0,0, 1,1,1,1, 0,0,0,0)

def geometry_fixture():
    data = bytearray()
    identity = (1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)
    for case in range(5):
        job = ((0,0,0,1),(0,0,0,0),(1,1,1,1),(1,1,1,1)) if not case else (
            (3*case,-4*case,5*case,1),(.031*case,-.077*case,.019*case,0),
            (.5+case/4,1.5,2,1),(.4,.6,.8,.5))
        camera = (0,0,0,1) if not case else (.123*case,-.231*case,.02*case,1)
        view = identity if case<3 else (0,0,1,0, 0,1,0,0, -1,0,0,0, 7,-3,11,1)
        data += struct.pack('<I37f', case, *(x for v in job for x in v), *camera, *view, 450)
        for i in range(2):
            values = ((i*.2,-i*.4,5+i*.1,0),(.025*i,.03*i,-.07*i,0),
                      (2.5,3.5,1,1),(.65,.55,.45,.75))
            data += struct.pack('<16f4I', *(x for v in values for x in v), 24,
                                0x40 if not i else 0x200, 0x41 if not i else 0x201, 20)
        data += struct.pack('<34I', *[305+7*v+case for i in range(2) for v in range(17)])
    return bytes(data)

def compare_uv(original, native, steps=323):
    if len(original) != steps*2*72*4 or len(native) != len(original):
        raise ValueError('Cylinder UV observation extent')
    records = list(struct.iter_unpack('<72I', original))
    for index, r in enumerate(records):
        step, element = divmod(index, 2)
        if r[:4] != (step, element, 0, 34) or r[5::2] != (0, 2)*17:
            raise ValueError('Cylinder UV observation identity/tags')
        if element and r[4:] != records[index-1][4:]:
            raise ValueError('Cylinder geometry sharing differs')
    if native != original:
        raise ValueError('Native cylinder UV differs')
    return steps*2*34

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, required=True)
    p.add_argument('--reference', type=Path, default=Path('local/research/effect26-v1'))
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    if digest(Path('local/extracted/SCUS_971.29')) != ELF_SHA:
        raise ValueError('Different retail ELF')
    write_exact(a.reference/'26.input.bin', state_fixture())
    write_exact(a.reference/'geometry-fixture-v1.bin', geometry_fixture())
    for name, sha in HASHES.items():
        if digest(a.reference/name) != sha:
            raise ValueError('Changed original cylinder evidence: '+name)
    manifests = [json.loads((a.reference/name).read_text()) for name in
                 ('26.original.bin.run.json', 'geometry-original-v1.bin.run.json')]
    if any(r['exit_code'] for r in manifests):
        raise ValueError('Incomplete original cylinder probe')
    commands = []
    efd = 'local/effect-input/CMNDATA/2D/TES.EFD'
    textures = 'local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF'
    for command, fixture, output in [
        ('map-effect-probe', '26.input.bin', 'state.bin'),
        ('cylinder26-effect-geometry', 'geometry-fixture-v1.bin', 'drawing.bin')]:
        cmd = [str(a.native), command, efd, textures, str(a.reference/fixture)]
        if command == 'map-effect-probe':
            cmd += ['321']
        cmd += [str(a.output/output)]
        r = subprocess.run(cmd, capture_output=True)
        write_exact(a.output/(output+'.log'), r.stdout+r.stderr)
        record = dict(command=cmd, exit_code=r.returncode, binary_sha256=digest(a.native))
        write_exact(a.output/(output+'.run.json'), (json.dumps(record, indent=2)+'\n').encode())
        r.check_returncode()
        commands.append(record)
    state = (a.output/'state.bin').read_bytes()
    if len(state) != 323*117*4 or state != (a.reference/'26.original.bin').read_bytes():
        raise ValueError('Native cylinder state differs')
    uv = compare_uv((a.reference/'26.original.bin.uv.bin').read_bytes(),
                    (a.output/'state.bin.uv.bin').read_bytes())
    matrices = (a.output/'drawing.bin').read_bytes()
    if len(matrices) != 5*41*4 or matrices != (a.reference/'geometry-original-v1.bin').read_bytes():
        raise ValueError('Cylinder transforms/colors differ')
    vertices = compare_vertices((a.reference/'geometry-original-v1.bin.packets.bin').read_bytes(),
                                (a.output/'drawing.bin.vertices.bin').read_bytes(), 26)
    if (a.output/'drawing.bin.materials.bin').read_bytes() != struct.pack('<6I', 0,0,1,1,1,1):
        raise ValueError('Cylinder native blend/depth mapping differs')
    report = dict(scope=__doc__, elf_sha256=ELF_SHA, original_sha256=HASHES,
                  original_runs=manifests, native_runs=commands,
                  state_words_exact=323*117, uv_entries_exact=uv,
                  transforms_colors_exact=10, vertices_exact=vertices,
                  material='ALPHA44/48, ZBUF mask1, TEST5000d/50000; both depth-tested')
    write_exact(a.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print('Cylinder26: state, shared UVs,10 transforms/colors and192 vertices exact')

if __name__ == '__main__':
    main()
