#!/usr/bin/env python3
"""Compare native shadow projection setup/matrices with original packet generation.

Explicit synthetic views, light directions, heights, modes and transition factors
feed both implementations. Original EE/VU math and packet construction execute
unchanged in a bounded arena. This does not execute the shadow VU1 renderer or
prove GS/Metal pixels, field integration or service54 completion.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import time
from verify_event import write_exact
from verify_map_effect import digest, ELF_SHA

def fixture():
    angle = .37
    c, s = math.cos(angle), math.sin(angle)
    views = [[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],
             [0,0,-1,0,0,1,0,0,1,0,0,0,10,-7,3,1],
             [c,s,0,0,-s,c,0,0,0,0,1,0,3.25,-10.25,17.5,1]]
    result = []
    for view in views:
        for light in [( .3,1,0,0), (1,1,0,0), (-.75,2,.125,0)]:
            cases = [(i, 1.) for i in range(8)]
            cases += [(4, f) for f in [.9999999403953552, .5, 0, -2.086162567138672e-7, 1.0499993562698364]]
            for mode, factor in cases:
                i = len(result)
                result.append(struct.pack('<2I22f3I', i, mode, [-10,0,13.25,80][i%4], factor,
                                          *light, *view, 17+i, 31, 192))
    return b''.join(result)

def packet(data):
    if len(data) < 80 or len(data) % 16:
        raise ValueError('Shadow packet extent')
    words = struct.unpack('<%dI' % (len(data)//4), data)
    if words[:4] != (0x10000000|(len(data)-32)//16, 0, 0, 0x1800000+len(data)):
        raise ValueError('Shadow packet DMA header')
    if words[-4:] != (0x60000000, 0, 0, 0):
        raise ValueError('Shadow packet DMA end')
    body = words[4:-4]
    if body[:6] != (0x11000000,0x50000002,0x8001,0x10000000,0xe,0) or body[7:10] != (0x3f800000,1,0):
        raise ValueError('Shadow packet RGBAQ command')
    if body[6] >> 24:
        raise ValueError('Shadow packet color alpha')
    color = tuple((body[6] >> (i*8)) & 255 for i in range(3))
    at = 10
    direction = None
    matrices = []
    if at < len(body) and body[at] == 0x6c018000:
        if len(body)-at < 6 or body[at+5] != 0x1400000a:
            raise ValueError('Shadow packet direction command')
        direction = body[at+1:at+5]
        at += 6
    while at < len(body) and body[at] == 0x6c048000:
        if len(matrices) == 2 or len(body)-at < 18 or body[at+17] != 0x14000006+2*len(matrices):
            raise ValueError('Shadow packet matrix command order')
        matrices.append(body[at+1:at+17])
        at += 18
    if len(matrices) not in (0,2) or len(body)-at > 3 or any(body[at:]):
        raise ValueError('Shadow packet trailing commands or incomplete matrices')
    return color, matrices, direction

def compare(original, native, original_setup, native_setup, inputs):
    if not inputs or len(inputs) % 108 or len(inputs) > 128*108:
        raise ValueError('Shadow projection fixture extent')
    fixtures = list(struct.iter_unpack('<2I22f3I', inputs))
    for case, f in enumerate(fixtures):
        if f[0] != case or f[1] > 7 or not all(math.isfinite(v) for v in f[2:24]) or any(v > 255 for v in f[24:]):
            raise ValueError('Shadow projection fixture scope')
    if not native or len(native) % (43*4):
        raise ValueError('Native shadow projection extent')
    rows = list(struct.iter_unpack('<43I', native))
    if len(rows) != len(fixtures):
        raise ValueError('Shadow projection fixture/record count')
    if len(original_setup) != len(rows)*57*4 or native_setup != original_setup:
        raise ValueError('Shadow projection setup differs')
    setup = list(struct.iter_unpack('<57I', original_setup))
    at = matrices_exact = directions_exact = 0
    for case, row in enumerate(rows):
        if len(original)-at < 12:
            raise ValueError('Original shadow projection header extent')
        identity, mode, size = struct.unpack_from('<3I', original, at)
        at += 12
        if identity != case or row[:2] != (identity,mode) or setup[case][0] != identity or mode != fixtures[case][1]:
            raise ValueError('Shadow projection identity')
        if size > len(original)-at:
            raise ValueError('Original shadow projection packet extent')
        color, matrices, direction = packet(original[at:at+size])
        at += size
        if bool(matrices) != (mode >= 4) or (direction is not None) != (mode == 4 and fixtures[case][3] != 1):
            raise ValueError('Original shadow projection mode/command contract')
        if color != (fixtures[case][24:27] if mode & 2 else (0,0,0)):
            raise ValueError('Original shadow projection tint contract')
        expected = (identity, mode, int(bool(matrices)), int(direction is not None), *color)
        expected += tuple(v for matrix in matrices for v in matrix) if matrices else (0,)*32
        expected += direction if direction is not None else (0,)*4
        if row != expected:
            diff = [(i, hex(a), hex(b)) for i,(a,b) in enumerate(zip(expected,row)) if a != b]
            raise ValueError(f'Shadow projection case{case} differs: {diff[:6]}')
        matrices_exact += len(matrices)
        directions_exact += direction is not None
    if at != len(original):
        raise ValueError('Original shadow projection trailing records')
    return dict(cases=len(rows), setup_words_exact=len(rows)*56,
                matrices_exact=matrices_exact, directions_exact=directions_exact,
                color_records_exact=len(rows))

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, required=True)
    p.add_argument('--oracle', type=Path, default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference', type=Path, default=Path('local/research/shadow-projection-v1'))
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    a.reference.mkdir(parents=True, exist_ok=True)
    root = a.reference
    inp = root/'projection-fixture-v1.bin'
    write_exact(inp, fixture())
    elf = Path('local/extracted/SCUS_971.29')
    if digest(elf) != ELF_SHA:
        raise ValueError('Different retail ELF')
    original = root/'projection-original-v1.bin'
    manifest = root/'projection-original-v1.run.json'
    if not original.exists():
        if manifest.exists():
            raise ValueError('Retained failed original probe; investigate before a fresh capture')
        cmd = [str(a.oracle), 'shadow-projection', str(elf), str(inp), str(original)]
        start = time.monotonic()
        with (root/'projection-original-v1.log').open('w') as log:
            result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
        run = dict(command=cmd, exit_code=result.returncode, elapsed_seconds=time.monotonic()-start,
                   binary_sha256=digest(a.oracle), elf_sha256=ELF_SHA, input_sha256=digest(inp))
        if not result.returncode:
            run['outputs'] = {path.name:digest(path) for path in [original, Path(str(original)+'.setup.bin')]}
        write_exact(manifest, (json.dumps(run, indent=2)+'\n').encode())
        if result.returncode:
            raise RuntimeError('Original shadow projection failed; retained manifest/log')
    run = json.loads(manifest.read_text())
    if run['exit_code'] or run['elf_sha256'] != ELF_SHA or run['input_sha256'] != digest(inp):
        raise ValueError('Changed/incomplete original shadow projection fixture')
    for name, sha in run['outputs'].items():
        if digest(root/name) != sha:
            raise ValueError('Changed original shadow projection output')
    native = a.output/'projection-native.bin'
    cmd = [str(a.native), 'shadow-projection', str(inp), str(native)]
    subprocess.run(cmd, check=True)
    result = compare(original.read_bytes(), native.read_bytes(),
                     Path(str(original)+'.setup.bin').read_bytes(), Path(str(native)+'.setup.bin').read_bytes(), inp.read_bytes())
    report = dict(scope=__doc__, original_run=run, native_command=cmd, native_sha256=digest(a.native), **result)
    write_exact(a.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(json.dumps(result))

if __name__ == '__main__':
    main()
