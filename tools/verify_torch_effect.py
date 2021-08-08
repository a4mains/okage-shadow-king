#!/usr/bin/env python3
"""Compare native torch91 state and mutable flame quads with original execution.

Four isolated cases cover a full9900-tick loop, a negative job-depth override,
axis-aligned and coincident camera positions. All state, packed flame XYZ and UV
words must match exactly. This excludes glow mesh, materials, camera-space draw
matrices, raster output, scene allocation/order and authentic startup RNG.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_map_effect import compare, digest, ELF_SHA

# name, ticks, input/state/quad hashes. Original snapshots are research-only.
CASES = [
    ('torch', 10000, '271aa3034dafcd2897e1223bcfe2fbf5767a6fefaf0b7c70d90535d21565ec4a',
     'afba159d83dd537b6446bb441d9c62f66cefdd371ae1b88705ac4f091828ec07',
     '8f116c812d254b111538e102fdb6851137b5dc8b6d4087ddd668d08230ed6e92'),
    ('torch-offset', 1000, 'dce8fcd205c39602ba6959899f7c4d08895c932beb528aa53e7f93e4ab27d8eb',
     'ff5bbbd23a42c7ec97bc6ae6c61a04717ab111e03520552a4f06e10809a58704',
     'fd767a14fb46ce2c2e6161bf90547c8349a46e03f156a9770f6fa14d111cedf0'),
    ('torch-axis', 1000, 'e25563ed33a507ab8e0a48be26b121f1c71e6002ea7d2c60c05ae7d7da6ca510',
     '8803458be9ff9e50a95003b30e9082f3064ffa57fa0bcbb662d252a828bca17a',
     'f515bca754910c387c941b7dbbc1e3c9c45bde441fec565b9e1f34bdb91bbb5b'),
    ('torch-coincident', 1000, '9891c81d855265b6808e7f1da0aa0dc644d469a1e8dd14f4192024e5d445a032',
     'f5fc629e1b3a5ab77dc2974d1f367ed12828dbe0831b715c9a6da26d97a42c2d',
     'f515bca754910c387c941b7dbbc1e3c9c45bde441fec565b9e1f34bdb91bbb5b'),
]

def compare_quads(original, native, frames):
    if len(original) != (frames + 2) * 160 or len(native) != len(original):
        raise ValueError('Incomplete torch quad interval')
    for step, row in enumerate(struct.iter_unpack('<40I', original)):
        if row[0] != step or tuple(row[1 + 13 * i] for i in range(3)) != (0, 1, 2):
            raise ValueError('Torch quad identity/order differs')
    if original != native:
        raise ValueError('Torch packed XYZ/UV differs; no tolerance allowed')
    return {'records': frames + 2, 'packed_position_uv_words_exact': (frames + 2) * 36}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--reference', type=Path, default=Path('local/research/interior-effects-v1'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    if digest(Path('local/extracted/SCUS_971.29')) != ELF_SHA:
        raise ValueError('Different retail ELF')
    efd = Path('local/effect-input/CMNDATA/2D/TES.EFD')
    textures = Path('local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF')
    report = {'scope': __doc__, 'elf_sha256': ELF_SHA, 'efd_sha256': digest(efd),
              'native_sha256': digest(args.native), 'cases': []}
    for name, frames, input_hash, state_hash, geometry_hash in CASES:
        fixture = args.reference / (name + '.input.bin')
        original = args.reference / (name + '.original-v2.bin')
        geometry = Path(str(original) + '.torch-geometry.bin')
        run = json.loads((args.reference / (name + '.v2.run.json')).read_text())
        if run['exit_code'] or tuple(map(digest, (fixture, original, geometry))) != (input_hash, state_hash, geometry_hash):
            raise ValueError('Incomplete or changed original torch probe')
        output = args.output / (name + '.native.bin')
        command = [str(args.native), 'map-effect-probe', str(efd), str(textures),
                   str(fixture), str(frames), str(output)]
        result = subprocess.run(command, capture_output=True)
        write_exact(args.output / (name + '.log'), result.stdout + result.stderr)
        write_exact(args.output / (name + '.run.json'),
                    (json.dumps({'command': command, 'exit_code': result.returncode,
                                 'binary_sha256': digest(args.native)}, indent=2) + '\n').encode())
        result.check_returncode()
        compare(original.read_bytes(), output.read_bytes(), 91, 4, frames)
        if original.read_bytes() != output.read_bytes():
            raise ValueError('Torch state is not bit exact')
        quad_result = compare_quads(geometry.read_bytes(), Path(str(output) + '.torch-geometry.bin').read_bytes(), frames)
        phases = [row[6] for row in struct.iter_unpack('<205I', original.read_bytes())]
        wraps = sum(b < a for a, b in zip(phases, phases[1:]))
        if name == 'torch' and wraps != 1:
            raise ValueError('Full-loop case must include exactly one phase wrap')
        report['cases'].append({'name': name, 'ticks': frames, 'records': frames + 2,
            'state_words_exact': (frames + 2) * 205, 'phase_wraps': wraps,
            'input_sha256': input_hash, 'state_sha256': state_hash,
            'quad_sha256': geometry_hash, 'quads': quad_result,
            'original_run': run, 'native_command': command})
    write_exact(args.output / 'report.json', (json.dumps(report, indent=2) + '\n').encode())
    print('Torch91 state and mutable flame XYZ/UV match exactly in all four cases')

if __name__ == '__main__':
    main()
