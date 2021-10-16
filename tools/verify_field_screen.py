#!/usr/bin/env python3
"""Compare native ordinary screen arithmetic and XYZF packing with original EE/VU.

Explicit unclipped camera vertices and direct screen-coordinate boundary cases
exercise original screen setup, ordinary VU1 cache0xe08 and triangle packing
0x1db0. This is not GS raster, clipping, field composition or hardware proof.
Decoded XYZF fields are interpretations of the original emitted packet.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import time
from verify_map_effect import ELF_SHA

INPUT_BYTES = 116
ORIGINAL_BYTES = 460
NATIVE_BYTES = 340

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def write_retained(path, data):
    """Create exclusively, or accept identical retained evidence."""
    if path.is_symlink():
        raise ValueError(f'Refusing symlink output: {path}')
    try:
        with path.open('xb') as stream:
            stream.write(data)
    except FileExistsError:
        if path.read_bytes() != data:
            raise ValueError(f'Refusing changed output: {path}')

def adjacent(value, delta):
    bits = struct.unpack('<I', struct.pack('<f', value))[0]
    return struct.unpack('<f', struct.pack('<I', bits + delta))[0]

def fixture():
    rng = random.Random(0x215900)
    rows = []

    def append(mode, settings, vertices):
        rows.append(struct.pack('<2I27f', len(rows), mode, *settings,
                                *(v for vertex in vertices for v in vertex)))

    for case in range(16):
        settings = [800, 1, .47, 2048, 2048, 0, 65536, 3, 2048,
                    1, 4096, 0, 0, 320, 112]
        if case >= 8:
            settings[0:3] = [rng.uniform(240, 960), rng.uniform(.6, 1.4), rng.uniform(.3, .8)]
            settings[3:5] = [rng.uniform(1960, 2090), rng.uniform(1980, 2090)]
            settings[5:9] = [101, 16777115, 3, [1024, 2048, 4096][case % 3]]
            settings[10] = 8192
        settings[11:13] = [(0, 0), (5, 500), (50, 1500)][case % 3]
        xy = [(settings[3] - 80 + .0625, settings[4] - 40 + .3125),
              (settings[3] + 80 + .0625, settings[4] - 40 + .3125),
              (settings[3] + 80 + .0625, settings[4] + 40 + .3125)]
        if case < 8:
            target = [1000, 4096, 10000, 32768][case % 4]
            near, far = settings[7:9]
            scale = near * far * (settings[6] - settings[5]) / (far - near)
            base = (settings[5] * far - settings[6] * near) / (far - near)
            depths = [scale / (adjacent(target, delta) - base) for delta in (-1, 0, 1)]
        else:
            depths = [rng.uniform(4, 900), rng.uniform(4, 1900), rng.uniform(4, 2900)]
        vertices = [((x - settings[3]) * z / (settings[0] * settings[1]),
                     (y - settings[4]) * z / (settings[0] * settings[2]), z, 1)
                    for (x, y), z in zip(xy, depths)]
        for mode in (0, 1):
            append(mode, settings, vertices)

    for case in range(16):
        settings = [800, 1, .47, 2048, 2048, 101, 16777115, 3, 2048,
                    1, 4096, 0, 0, 320, 112]
        target = [.0625, 1, 255, 10000, 65536, 1048576, 8388608, 16777214][case % 8]
        depths = [adjacent(target, delta) for delta in (-1, 0, 1)]
        if case == 8:
            depths = [0, .0625, adjacent(1, -1)]
        shift = [-1, 0, 1][case % 3]
        vertices = [(adjacent(1992.0625, shift), adjacent(2016.3125, -shift), depths[0], 0),
                    (adjacent(2072.0625, shift), adjacent(2016.3125, -shift), depths[1], 127.9375),
                    (adjacent(2072.0625, shift), adjacent(2072.3125, -shift), depths[2], 255)]
        for mode in (2, 3):
            append(mode, settings, vertices)
    return b''.join(rows)

def compare(original, native, inputs):
    if not inputs or len(inputs) % INPUT_BYTES or len(inputs) > 64 * INPUT_BYTES:
        raise ValueError('Field screen fixture extent')
    count = len(inputs) // INPUT_BYTES
    if len(original) != count * ORIGINAL_BYTES or len(native) != count * NATIVE_BYTES:
        raise ValueError('Field screen output extent')
    modes = [0] * 4
    for case, row in enumerate(struct.iter_unpack('<2I27f', inputs)):
        identity, mode = row[:2]
        if identity != case or mode > 3 or not all(math.isfinite(v) for v in row[2:]):
            raise ValueError('Field screen fixture identity/mode/finite scope')
        expected = struct.unpack_from('<115I', original, case * ORIGINAL_BYTES)
        actual = struct.unpack_from('<85I', native, case * NATIVE_BYTES)
        if expected[:4] != (identity, mode, 0, 10):
            raise ValueError(f'Original field screen case{case} identity/mode/status/emission')
        if expected[60:63] != (0, 0, 0):
            raise ValueError(f'Original field screen case{case} clip flags')
        if mode & 2:
            screen = struct.unpack_from('<12I', inputs, case * INPUT_BYTES + 17 * 4)
            if any(expected[36:48]) or expected[48:60] != screen:
                raise ValueError(f'Original field screen case{case} direct input/cache')
        prim = 0x5d if mode & 1 else 0x4d
        header = (3, 0x30004000 | (prim << 15) | prim, 0x412 if mode & 1 else 0x41f, 0)
        if expected[63:67] != header:
            raise ValueError(f'Original field screen case{case} GIF header')
        packed = tuple(word for i in range(3) for word in expected[75 + 12*i:79 + 12*i])
        decoded = tuple(word for i in range(3) for word in
                        (packed[i*4] & 0xffff, packed[i*4+1] & 0xffff,
                         (packed[i*4+2] & 0x0ffffff0) >> 4, (packed[i*4+3] & 0xff0) >> 4))
        if expected[103:115] != decoded:
            raise ValueError(f'Original field screen case{case} XYZF decoding')
        mapped = expected[:2] + expected[4:63] + packed + expected[103:115]
        if mapped != actual:
            differences = [(i, hex(a), hex(b)) for i, (a, b) in enumerate(zip(mapped, actual)) if a != b]
            raise ValueError(f'Field screen case{case} differs: {differences[:8]}')
        modes[mode] += 1
    return dict(cases=count, mode_counts=modes, setup_words_exact=count * 32,
                camera_cache_vertices_exact=sum(modes[:2]) * 3,
                direct_screen_vertices_exact=sum(modes[2:]) * 3,
                packed_words_exact=count * 12, decoded_words_exact=count * 12,
                data_words_exact=count * 83)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--oracle', type=Path, default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    parser.add_argument('--reference', type=Path, default=Path('local/research/field-screen-v1'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.is_symlink() or args.reference.is_symlink():
        raise ValueError('Refusing symlink result directory')
    args.output.mkdir(parents=True, exist_ok=False)
    args.reference.mkdir(parents=True, exist_ok=True)
    inputs = args.reference / 'fixture-v1.bin'
    write_retained(inputs, fixture())
    elf = Path('local/extracted/SCUS_971.29')
    if digest(elf) != ELF_SHA:
        raise ValueError('Different retail ELF')
    original = args.reference / 'original-v1.bin'
    manifest = args.reference / 'original-v1.run.json'
    log = args.reference / 'original-v1.log'
    if any(path.is_symlink() for path in (original, manifest, log)):
        raise ValueError('Refusing symlink original evidence')
    if not original.exists():
        if manifest.exists() or log.exists():
            raise ValueError('Retained original attempt; investigate before retry')
        command = [str(args.oracle), 'ordinary-depth', str(elf), str(inputs), str(original)]
        binary_sha = digest(args.oracle)
        start = time.monotonic()
        with log.open('x') as stream:
            process = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
        run = dict(command=command, exit_code=process.returncode,
                   elapsed_seconds=time.monotonic() - start, binary_sha256=binary_sha,
                   elf_sha256=ELF_SHA, input_sha256=digest(inputs))
        if not process.returncode:
            run['output_sha256'] = digest(original)
        write_retained(manifest, (json.dumps(run, indent=2) + '\n').encode())
        if process.returncode:
            raise RuntimeError('Original field screen probe failed')
    if not manifest.exists():
        raise ValueError('Original field screen output lacks a run manifest')
    run = json.loads(manifest.read_text())
    if (run.get('exit_code') != 0 or run.get('elf_sha256') != ELF_SHA or
            run.get('input_sha256') != digest(inputs) or run.get('output_sha256') != digest(original) or
            run.get('command', [])[1:2] != ['ordinary-depth'] or
            len(run.get('binary_sha256', '')) != 64):
        raise ValueError('Changed/incomplete original field screen evidence')
    native = args.output / 'native.bin'
    command = [str(args.native), 'field-screen', str(inputs), str(native)]
    native_sha = digest(args.native)
    with (args.output / 'native.log').open('x') as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)
    result = compare(original.read_bytes(), native.read_bytes(), inputs.read_bytes())
    report = dict(scope=__doc__, original_run=run, native_command=command,
                  native_sha256=native_sha, native_output_sha256=digest(native), **result)
    write_retained(args.output / 'report.json', (json.dumps(report, indent=2) + '\n').encode())
    print(json.dumps(result))

if __name__ == '__main__':
    main()
