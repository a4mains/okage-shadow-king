#!/usr/bin/env python3
"""Audit integrated native kitchen movement against paired original observations.

Original rendering precedes the player callback within a captured vblank.
Compare callback state with native post-update state; rendered roots precede
movement, while pose channels follow that frame's scene evaluation. The legacy
option retains the old runtime's comparison for historical captures. Derive the
time anchor from handoff/counter state, never by minimizing spatial errors.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
from prepare_assets import ELF_HASH
from verify_event import write_exact
from verify_choice_scene import rows, same_family

def floats(words):
    return struct.unpack('<%df' % len(words), struct.pack('<%dI' % len(words), *words))

def error(a, b):
    a, b = floats(a), floats(b)
    if not all(math.isfinite(x) for x in a + b):
        raise RuntimeError('Non-finite movement observation')
    return max(abs(x-y) for x, y in zip(a, b))

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference', type=Path, default=Path('local/reference-kitchen-movement-180-v2'))
    p.add_argument('--capture', type=Path, default=Path('local/native-movement-6000-debug-v4.png'))
    p.add_argument('--compare', type=Path, action='append', default=[])
    p.add_argument('--ui', type=Path)
    p.add_argument('--schedule',choices=['legacy','current'],default='legacy')
    p.add_argument('--output', type=Path, default=Path('local/movement-scene-validation-v1.json'))
    args = p.parse_args()
    ref = lambda suffix: Path(str(args.reference) + suffix)
    native = lambda suffix: Path(str(args.capture) + suffix)
    if hashlib.sha256(Path('local/extracted/SCUS_971.29').read_bytes()).hexdigest() != ELF_HASH:
        raise RuntimeError('Unexpected original ELF')
    if 'Stopped: vblanks=180 exit_request=0' not in ref('.log').read_text():
        raise RuntimeError('Original movement capture is not complete')
    observed = rows(ref('.movement.bin'), 64)
    before = [r for r in observed if r[1] == 0x1354d8]
    after = [r for r in observed if r[1] == 0x134b70]
    sounds = [r for r in observed if r[1] == 0x12df80]
    actors, cameras = rows(ref('.actor.bin'), 920), rows(ref('.camera.bin'), 32)
    if any(len(part) != 180 for part in (before, after, actors, cameras)):
        raise RuntimeError('Incomplete original phase coverage')
    for part in (before, after, actors, cameras):
        if [r[0] for r in part] != list(range(180)):
            raise RuntimeError('Original observations are not one per frame')
    moves = rows(native('.movement.bin'), 16)
    camera_states = rows(native('.camera-state.bin'), 5)
    ncameras = rows(native('.camera.bin'), 16)
    raw = native('.actor-1.bin').read_bytes()
    header = struct.unpack_from('<8I', raw)
    if header[5] != 228:
        raise RuntimeError('Unexpected native player channel count')
    nactors = rows(native('.actor-1.bin'), 8 + header[5]*4 + header[6])
    for part in (moves, camera_states, ncameras, nactors):
        if len(part) != 6000 or [r[0] for r in part] != list(range(1, 6001)):
            raise RuntimeError('Incomplete native observation sequence')
    handoff = next(i for i, r in enumerate(camera_states) if r[1] == 0)
    current_schedule=args.schedule=='current'
    first_update = camera_states[handoff+(1 if current_schedule else 2)][0]
    if camera_states[handoff+2][1] != 2 or moves[first_update-1][11] != 1 or moves[first_update-1][2]&1:
        raise RuntimeError('Unexpected native camera/player handoff')
    if before[0][4] or before[0][10] or before[0][11] != 1 or before[0][12]:
        raise RuntimeError('Expected idle original entry anchor')
    origin = first_update + before[0][19]
    if moves[origin-1][11] != before[0][19]+1:
        raise RuntimeError('Original/native idle counter anchor differs')
    maximum_position = maximum_camera = maximum_target = 0.
    motion_changes=[]
    for frame, (entry, post, actor, camera) in enumerate(zip(before, after, actors, cameras)):
        m = moves[origin+frame-1]
        expected = (post[10], post[9], post[11], post[12], post[13], post[14],
                    post[15], post[16], post[17], post[19], post[20], post[21], post[18], post[30])
        if m[2:] != expected or (m[1] & 0xf000) != (entry[4] & 0xf000):
            raise RuntimeError('Player state/input differs at original frame %d' % frame)
        current = nactors[origin+frame-1]
        maximum_position = max(maximum_position, error(current[12:16], post[48:52]))
        if current[8:12] != post[56:60]:
            raise RuntimeError('Post-movement facing differs')
        rendered = nactors[origin+frame-2]
        pose = current if current_schedule else rendered
        changed=current_schedule and entry[11]!=post[11]
        if changed:motion_changes.append(frame)
        if ((not changed and pose[2:4] != actor[2:4]) or
            rendered[8:12] != actor[8:12] or pose[16:920] != actor[16:920]):
            raise RuntimeError('Rendered script, facing, scale or pose channels differ at frame %d' % frame)
        maximum_position = max(maximum_position, error(rendered[12:16], actor[12:16]))
        if frame and actor[12:16] != after[frame-1][48:52]:
            raise RuntimeError('Original render/player phase relationship changed')
        nc = ncameras[origin+frame-(1 if current_schedule else 2)]
        if nc[12:16] != camera[8:12] or camera[2:4] != (2, 0x3f800000):
            raise RuntimeError('Rendered camera rotation/mode/phase differs')
        maximum_camera = max(maximum_camera, error(nc[4:8], camera[4:8]))
        maximum_target = max(maximum_target, error(nc[8:12], camera[12:16]))
    if max(maximum_position, maximum_camera, maximum_target) > .00002:
        raise RuntimeError('Integrated movement/camera exceeds existing host-math tolerance')
    native_sounds = [r for r in rows(native('.sound.bin'), 5) if r[1] == 0]
    expected_sounds = [(origin+r[0], 0, r[3], 0x3f800000, 60) for r in sounds]
    if any(r[3] != 35 or r[2] != 0x1350b0 for r in sounds) or native_sounds != expected_sounds or not sounds:
        raise RuntimeError('Footstep calls differ from original movement caller')
    suffixes = ['', '.json', '.audio.json', '.audio.pcm', '.music.bin', '.sound.bin',
                '.camera.bin', '.camera-state.bin', '.movement.bin', '.actor.bin',
                '.actor-9.bin', '.actor-1.bin', '.dialogue.bin', '.choices.bin', '.effects.bin']
    for capture in args.compare:
        same_family(args.capture, capture, suffixes)
    ui = None
    if args.ui:
        same_family(args.capture, args.ui, [s for s in suffixes if s != '.audio.json'])
        audio = json.loads(Path(str(args.ui)+'.audio.json').read_text())
        expected_audio = json.loads(native('.audio.json').read_text())
        for key in expected_audio:
            if not key.startswith('device_') and audio[key] != expected_audio[key]:
                raise RuntimeError('UI software audio metadata differs')
        if audio['device_frames_enqueued'] <= 0:
            raise RuntimeError('UI replay did not submit audio')
        ui = {'capture': str(args.ui), 'headless_image_state_pcm_traces_exact': True,
              'device_frames_enqueued': audio['device_frames_enqueued'],
              'device_underrun_frames': audio['device_underrun_frames'],
              'physical_input_or_listening_verified': False}
    paths = [ref(s) for s in ('.movement.bin', '.actor.bin', '.camera.bin', '.log', '.state.zip')]
    paths += [native(s) for s in suffixes]
    report = {
        'scope': __doc__.strip(), 'elf_sha256': ELF_HASH, 'original_frames': 180,
        'native_frames': 6000, 'native_first_player_update': first_update,
        'native_frame_for_original_callback_zero': origin,
        'phase_evidence': 'Original rendered position at N equals post-player position at N-1 for every observed N>0. Current runtime pose/camera evaluate before movement in the same frame.',
        'schedule':args.schedule,
        'player_state_input_exact': True, 'rendered_nonposition_channels_exact': True,
        'exact_rendered_script_samples':180-len(motion_changes),
        'script_phase_exclusions':motion_changes,
        'script_phase_limit':'At motion-selection callbacks, original render script state precedes selection; native post-update state follows selection. These states are not compared.',
        'rotation_exact': True, 'camera_rotation_exact': True,
        'position_maximum_absolute_error': maximum_position,
        'camera_position_maximum_absolute_error': maximum_camera,
        'camera_target_maximum_absolute_error': maximum_target,
        'position_tolerance': .00002,
        'float_limit': 'Existing host sine/libm tolerance; the first divergent movement uses table entries differing by one float ULP.',
        'footstep_calls_exact': native_sounds,
        'matching_native_captures': [str(x) for x in args.compare],
        'appkit': ui,
        'unverified': ['general maps/analog controls', 'contact bubble and nonzero dust effect',
                       'whole-game initialization', 'collision-history initialization',
                       'physical controller/listening', 'original stream startup latency/wet routing',
                       'exact final scene presentation', 'interactions/transitions/battles/saves/full playthrough'],
        'sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}}
    write_exact(args.output, (json.dumps(report, indent=2)+'\n').encode())
    print('180 paired player updates, rendered channels/camera and %d footsteps verified' % len(sounds))

if __name__ == '__main__':
    main()
