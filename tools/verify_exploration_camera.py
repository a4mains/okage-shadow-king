#!/usr/bin/env python3
"""Audit anchored room-camera behavior and the native kitchen camera handoff."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_choice_scene import rows, same_family

def floats(words):
    return struct.unpack('<%df' % len(words), struct.pack('<%dI' % len(words), *words))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--output', type=Path, default=Path('local/exploration-camera-validation'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    elf = Path('local/extracted/SCUS_971.29')
    elf_hash = hashlib.sha256(elf.read_bytes()).hexdigest()
    if elf_hash != '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5':
        raise RuntimeError('Unexpected retail ELF')
    original = Path('local/reference-kitchen-movement-180-v1')
    actor_path = Path(str(original) + '.actor.bin')
    camera_path = Path(str(original) + '.camera.bin')
    actors, cameras = rows(actor_path, 920), rows(camera_path, 32)
    if 'Stopped: vblanks=180 exit_request=0' not in Path(str(original) + '.log').read_text():
        raise RuntimeError('Original movement capture did not finish')
    if len(actors) != 180 or len(cameras) != 180:
        raise RuntimeError('Unexpected movement observation count')
    for frame, (actor, camera) in enumerate(zip(actors, cameras)):
        if actor[0] != frame or camera[0] != frame or actor[5] != 228 or camera[2:4] != (2, 0x3f800000):
            raise RuntimeError('Unpaired movement/camera state')
    if actors[0][12:16] == actors[60][12:16] or actors[60][12:16] == actors[160][12:16]:
        raise RuntimeError('Both original movement intervals must change position')
    fixture = args.output / 'positions.bin'
    write_exact(fixture, b''.join(struct.pack('<4I', *a[12:16]) for a in actors))
    stage = Path('local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF/a.stg')
    output = args.output / 'native.bin'
    subprocess.run([str(args.native), 'camera-anchored', str(stage), str(fixture), str(output)], check=True)
    result = rows(output, 12)
    if len(result) != 180:
        raise RuntimeError('Unexpected native camera count')
    maximum = 0.
    for native, reference in zip(result, cameras):
        if native[4:8] != reference[12:16] or native[8:12] != reference[8:12]:
            raise RuntimeError('Camera target or quantized rotation differs')
        positions = floats(native[:4]) + floats(reference[4:8])
        if not all(math.isfinite(v) for v in positions):
            raise RuntimeError('Non-finite camera position')
        error = max(abs(a-b) for a, b in zip(positions[:4], positions[4:]))
        if error > 0.00002:
            raise RuntimeError('Camera position exceeds float tolerance')
        maximum = max(maximum, error)

    prefix = Path('local/native-exploration-camera-6000-debug-v2.png')
    suffixes = ['', '.json', '.audio.json', '.audio.pcm', '.music.bin', '.sound.bin',
                '.camera.bin', '.camera-state.bin', '.actor.bin', '.actor-9.bin',
                '.actor-1.bin', '.dialogue.bin', '.choices.bin', '.effects.bin']
    for variant in ('release', 'sanitize'):
        same_family(prefix, Path('local/native-exploration-camera-6000-' + variant + '-v2.png'), suffixes)
    old = Path('local/native-kitchen-end-6000-v1.png')
    preserved = ['.audio.pcm', '.music.bin', '.sound.bin', '.actor.bin', '.actor-9.bin',
                 '.actor-1.bin', '.dialogue.bin', '.choices.bin', '.effects.bin']
    same_family(prefix, old, preserved)
    state = json.loads(Path(str(prefix) + '.json').read_text())
    if state['field_event_running'] or state['field_camera_mode'] != 2 or state['frames'] != 6000:
        raise RuntimeError('Native camera did not take over after event end')
    native_camera = rows(Path(str(prefix) + '.camera.bin'), 16)
    native_states = rows(Path(str(prefix) + '.camera-state.bin'), 5)
    partial_path = Path('local/reference-kitchen-end-700-v1.camera.bin')
    partial = rows(partial_path, 32)
    first_original = next(i for i, r in enumerate(partial) if r[2] == 0)
    first_native = next(i for i, r in enumerate(native_states) if r[1] == 0)
    if len(native_states) != 6000 or len(native_camera) != 6000 or len(partial) != 666:
        raise RuntimeError('Unexpected handoff trace extent')
    if any(a[0] != frame or b[0] != frame for frame, (a, b) in enumerate(zip(native_camera, native_states), 1)):
        raise RuntimeError('Unpaired native camera/state rows')
    if native_states[first_native][2] or native_states[first_native+2][1] != 2:
        raise RuntimeError('Native event end/camera-manager ordering changed')
    for offset, ref in enumerate(partial[first_original:]):
        at = first_native + offset
        cam, status = native_camera[at], native_states[at]
        if cam[4:8] + cam[12:16] != ref[4:12] or (status[1], status[3]) != (ref[2], ref[3]):
            raise RuntimeError('Original/native handoff position, rotation, mode or phase differs')
    old_camera = Path(str(old) + '.camera.bin').read_bytes()
    if Path(str(prefix) + '.camera.bin').read_bytes()[:(first_native+2)*64] != old_camera[:(first_native+2)*64]:
        raise RuntimeError('Earlier native camera path changed')
    final_expected = cameras[0][4:8] + cameras[0][12:16] + cameras[0][8:12]
    if native_camera[-1][4:] != final_expected:
        raise RuntimeError('Settled camera differs from original pre-movement state')
    paths = [stage, actor_path, camera_path, partial_path, fixture, output,
             Path(str(prefix) + '.camera.bin'), Path(str(prefix) + '.camera-state.bin')]
    paths += [Path(str(original) + suffix) for suffix in ('.input', '.log', '.state.zip', '.layer0.png')]
    report = {
        'scope': "STG anchored camera and native post-event handoff; not native movement or full scene fidelity",
        'elf_sha256': elf_hash,
        'native_sha256': hashlib.sha256(args.native.read_bytes()).hexdigest(),
        'original_functions': ['0x138c98', '0x138990', '0x138a50', '0x136aa8', '0x133a98'],
        'original_movement_frames': 180, 'target_and_rotation_exact': True,
        'position_maximum_absolute_error': maximum, 'position_tolerance': 0.00002,
        'float_limit': 'Host sine/sqrt arithmetic; quantized rotation must still match exactly',
        'handoff': {'original_start_frame': partial[first_original][0],
                    'native_start_frame': native_states[first_native][0],
                    'frames_exact': len(partial)-first_original,
                    'compared': ['position', 'rotation', 'mode', 'blend phase'],
                    'target_note': 'Original observer records the unblended global target; native trace records the effective blended target. Do not compare them during blending.',
                    'reference_limit': 'v1 stopped at trace guard; only its complete camera rows are compared',
                    'settled_camera_exact': True},
        'native_scene': {'frames': 6000, 'all_three_builds_exact': True,
                         'preserved_previous_traces': preserved,
                         'physical_input_or_listening_verified': False},
        'hashes': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}
    write_exact(args.output / 'report.json', (json.dumps(report, indent=2) + '\n').encode())
    print('180 original movement-camera samples and native camera handoff verified')

if __name__ == '__main__':
    main()
