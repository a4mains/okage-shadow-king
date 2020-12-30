#!/usr/bin/env python3
"""Verify the bounded native HOME1C camera/dialogue/renderer integration on macOS."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(*args):
    subprocess.run([str(a) for a in args], check=True)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=Path('build/native'))
    p.add_argument('--output', type=Path, default=Path('local/field-validation'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    native = args.build/'okage-assets'
    app = args.build/'okage-native.app/Contents/MacOS/okage-native'
    oracle = Path('build/play-research/okage-oracle/okage-pose-oracle')
    elf = Path('local/extracted/SCUS_971.29')
    ram = Path('local/research/opening600-ee.bin')
    script = Path('local/xpf-decoded/FLDDATA/EVENT/EVDATA.XPF/e_home1c.xmt')
    map_path = Path('local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF/a.xmp')
    run(native, 'camera-angles', args.output/'angles.native.bin')
    run(oracle, 'camera-angles', elf, args.output/'angles.retail.bin')
    if (args.output/'angles.native.bin').read_bytes() != (args.output/'angles.retail.bin').read_bytes():
        raise ValueError('Original angle-table mismatch')
    rng = random.Random(497)
    cases = [(7.75, -6.71, -20.12, 1, -12.35, -2, -4.35, 1),
             (15.21, -5.71, -15.35, 1, -8.38, -2, 1.47, 1)]
    cases += [(x, y, z, 1, 0, 0, 0, 1) for x, y, z in
              [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, -1), (-1, 0, 0), (1, 1, 1)]]
    cases += [tuple([rng.uniform(-50, 50) for _ in range(3)]+[1]+
                    [rng.uniform(-50, 50) for _ in range(3)]+[1]) for _ in range(120)]
    fixture = args.output/'look-at-input.bin'
    fixture.write_bytes(b''.join(struct.pack('<8f', *c) for c in cases))
    run(native, 'camera-look-at', fixture, args.output/'look-at.native.bin')
    run(oracle, 'camera-look-at', elf, ram, fixture, args.output/'look-at.retail.bin')
    if (args.output/'look-at.native.bin').read_bytes() != (args.output/'look-at.retail.bin').read_bytes():
        raise ValueError('Original look-at mismatch')
    run(native, 'field-projection', map_path.with_suffix('.stg'), args.output/'projection.bin')
    projection = struct.unpack('<32I', Path('local/reference-scene-602-v2/0000.projection.bin').read_bytes())
    expected = struct.pack('<4I', *(projection[i] for i in (0, 1, 2, 6)))
    if (args.output/'projection.bin').read_bytes() != expected:
        raise ValueError('Original projection coefficients mismatch')
    for frames in (1, 360):
        run(app, 'local/font-input/CMNDATA/FONT', 'local/text-input/FLDDATA/MES/FIELD.PME',
            '--scene', map_path, '--event', script, '--frames', frames,
            '--press', '100,140,180,220,260,300,340', '--capture', args.output/('frame-%d.png' % frames))
    state = json.loads((args.output/'frame-360.png.json').read_text())
    native_rows = list(struct.iter_unpack('<16I', (args.output/'frame-360.png.camera.bin').read_bytes()))
    capture = Path('local/reference-camera-800-v1.bin')
    retail = list(struct.iter_unpack('<32I', capture.read_bytes()))
    if len(retail) != 200 or [r[0] for r in retail] != list(range(200)):
        raise ValueError('Incomplete original camera capture')
    if any(r[1:3] != (0x1b9c74, 2) for r in retail):
        raise ValueError('Original camera scope mismatch')
    original_release = next(r[0] for r in retail if r[3] != 0x3f800000)
    offset = state['wait_release_frame']-1-original_release
    if offset < 0 or offset+len(retail) > len(native_rows):
        raise ValueError('Camera trace alignment bounds')
    for i, r in enumerate(retail):
        n = native_rows[offset+i]
        if r[4:12] != n[4:8]+n[12:16]:
            raise ValueError('Original camera position/rotation mismatch at %d' % i)
    if state['wait_release_frame'] != 227 or native_rows[-1][1:3] != (0xbe4, 0x36):
        raise ValueError('Unexpected native event continuation')
    report = {
        'scope': "Native HOME1C segment 0xb88 through dialogue-gated camera move; event remains pending at actor opcode 0x36, not a playable scene or cold boot",
        'native_tool_sha256': digest(native), 'native_app_sha256': digest(app),
        'elf_sha256': digest(elf), 'script_sha256': digest(script), 'stg_sha256': digest(map_path.with_suffix('.stg')),
        'angle_table_entries': 4096, 'angle_table_exact': True,
        'angle_table_sha256': digest(args.output/'angles.native.bin'),
        'look_at_cases': len(cases), 'look_at_exact': True,
        'look_at_sha256': digest(args.output/'look-at.native.bin'),
        'projection_coefficients_exact_capture': True,
        'projection_sha256': digest(args.output/'projection.bin'),
        'continuous_camera': {'frames': len(retail), 'exact_words_per_frame': 8,
                              'reference_release_frame': original_release,
                              'native_release_frame': state['wait_release_frame'],
                              'native_row_offset': offset, 'position_rotation_exact': True,
                              'capture_sha256': digest(capture),
                              'native_trace_sha256': digest(args.output/'frame-360.png.camera.bin')},
        'native_state': state,
        'limits': ['Harness starts at the recovered camera segment, not full game startup',
                   'Ready dialogue layers are assumed; flat text panel is diagnostic',
                   'Actor animation service remains pending at 0xbe4; no opcode is skipped',
                   '26 effects, actors, faithful lighting/window presentation, audio, battle and saves remain open',
                   'Full-game camera reference is emulator research; no reference state feeds the native app']}
    (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
