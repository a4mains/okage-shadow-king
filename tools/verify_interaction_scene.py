#!/usr/bin/env python3
"""Audit native approach/talk/return captures against bounded original observations.

This verifies target/event dispatch, root transforms, movement gating and dialogue
progress. It does not claim full window initialization, bone-transition timing,
audio fidelity, cold startup, scene transitions or a playable complete game.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from prepare_assets import ELF_HASH
from verify_choice_scene import rows, same_family
from verify_event import write_exact
from verify_movement_scene import error

def actor_rows(path):
    header = struct.unpack_from('<8I', path.read_bytes())
    return rows(path, 8 + header[5]*4 + header[6])

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture', type=Path, default=Path('local/native-talk-end-5530-debug-v3.png'))
    p.add_argument('--compare', type=Path, action='append', default=[])
    p.add_argument('--ui', type=Path)
    p.add_argument('--database', type=Path,
                   default=Path('local/xpf-decoded/FLDDATA/EVENT/EVDATA.XPF/fldset.xdb'),
                   help='Database used by this capture; historical default predates runtime disc-database correction')
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    original = Path('local/reference-kitchen-talk-180-v1')
    continuation = Path('local/reference-kitchen-talk-end-300-v1')
    source = lambda prefix, suffix: Path(str(prefix)+suffix)
    native = lambda suffix: source(args.capture, suffix)
    if hashlib.sha256(Path('local/extracted/SCUS_971.29').read_bytes()).hexdigest() != ELF_HASH:
        raise RuntimeError('Unexpected original ELF')
    for prefix, frames in [(original, 180), (continuation, 300)]:
        if f'Stopped: vblanks={frames} exit_request=0' not in source(prefix, '.log').read_text():
            raise RuntimeError('Incomplete original interaction reference')
    observed = rows(source(original, '.interaction.bin'), 128)
    before = [r for r in observed if r[1] == 0x133a98]
    if [r[0] for r in before] != list(range(180)):
        raise RuntimeError('Missing original field-manager observations')
    selected = [(r[0], r[6]) for r in observed if r[1] == 0x131cac]
    invoked = [(r[0], r[3]) for r in observed if r[1] == 0x1bb7e0]
    if selected != [(40, 0)] or invoked != [(40, 30)]:
        raise RuntimeError('Unexpected original target/event selection')
    hero = actor_rows(native('.actor-1.bin'))
    mother = actor_rows(native('.actor.bin'))
    interactions = rows(native('.interaction.bin'), 8)
    movement = rows(native('.movement.bin'), 16)
    cameras = rows(native('.camera.bin'), 16)
    dialogue = rows(native('.dialogue.bin'), 12)
    if any(len(r) != 5530 for r in (hero, mother, interactions, movement, cameras, dialogue)):
        raise RuntimeError('Incomplete native capture')
    origin = 5050
    started = next(r for r in interactions if r[2])
    if started[:6] != (5090, 0x40, 0xfffffff8, 30, 1, 1):
        raise RuntimeError('Native target/event/input/movement gate differs')
    maximum_position = 0.
    for r in before:
        i = origin+r[0]-2
        if cameras[i][1] != r[21] or movement[i][2] != r[15]:
            raise RuntimeError('Field event PC or movement gate differs')
        for actor, offset in [(hero, 24), (mother, 40)]:
            if actor[i][8:12] != r[offset:offset+4]:
                raise RuntimeError('Actor root facing differs')
            maximum_position = max(maximum_position, error(actor[i][12:16], r[offset+4:offset+8]))
        if (movement[i+1][1] & 0xf000) != (r[7] & 0xf000):
            raise RuntimeError('Native direction input differs from observed original latch')
    if maximum_position > .00002:
        raise RuntimeError('Root positions exceed existing host-math tolerance')
    controller = rows(source(original, '.controller.bin'), 64)
    after = [r for r in controller if r[1] == 0x11ff38]
    if [r[0] for r in after] != list(range(40, 180)):
        raise RuntimeError('Missing original message-controller returns')
    stage_differences = []
    for r in after:
        n = dialogue[origin+r[0]-1]
        if n[1] != 10026 or n[6:8] != (r[36], r[16]):
            raise RuntimeError('Message/progress/advance count differs')
        if n[10] != r[7]:
            stage_differences.append([r[0], r[7], n[10]])
    if stage_differences != [[40, 2, 5], [41, 4, 5]]:
        raise RuntimeError('Known diagnostic window initialization difference changed')
    end = next(r for r in interactions if r[2] and not r[4])
    released = next(r for r in interactions if r[2] and not r[5])
    if released[0] != end[0]+1 or released[6] != interactions[5088][6]:
        raise RuntimeError('Native return-to-control/gate restoration failed')
    if hero[5498][12:16] == hero[5510][12:16] or any(r[5] for r in interactions[5499:5510]):
        raise RuntimeError('Native movement did not resume after conversation')
    ended_reference = rows(source(continuation, '.interaction.bin'), 128)
    handoffs = [r[0] for r in ended_reference if r[1] == 0x132ca8]
    if len(handoffs) != 1:
        raise RuntimeError('Original conversation did not return to field control exactly once')
    if origin+180+handoffs[0] != released[0]:
        raise RuntimeError('Original/native return-to-control timing differs')
    continued_frames = [r for r in ended_reference if r[1] == 0x133a98]
    if [r[0] for r in continued_frames] != list(range(300)):
        raise RuntimeError('Incomplete original continuation')
    for r in continued_frames:
        i = origin+180+r[0]-2
        if cameras[i][1] != r[21] or movement[i][2] != r[15]:
            raise RuntimeError('Continuation event PC or movement gate differs')
        for actor, offset in [(hero, 24), (mother, 40)]:
            if actor is hero and r[0] >= 270:
                continue
            if actor[i][8:12] != r[offset:offset+4]:
                raise RuntimeError('Continuation root facing differs')
            maximum_position = max(maximum_position, error(actor[i][12:16], r[offset+4:offset+8]))
    if maximum_position > .00002:
        raise RuntimeError('Continuation root position exceeds host-math tolerance')
    continued_controller = [r for r in rows(source(continuation, '.controller.bin'), 64) if r[1] == 0x11ff38]
    if [r[0] for r in continued_controller] != list(range(220)):
        raise RuntimeError('Incomplete original controller closure sequence')
    for r in continued_controller:
        n = dialogue[origin+180+r[0]-1]
        if n[1] != 10026 or n[6:8] != (r[36], r[16]) or n[10] != r[7]:
            raise RuntimeError('Continuation message/progress/advance/stage differs')
    rendered_mother = rows(source(continuation, '.actor.bin'), 656)
    if [r[0] for r in rendered_mother] != list(range(300)):
        raise RuntimeError('Incomplete original Mother render trace')
    for r in rendered_mother:
        n = mother[origin+180+r[0]-1]
        if r[2:4] != n[2:4] or r[8:656] != n[8:656]:
            raise RuntimeError('Mother script state or pose differs through conversation closure')
    state = json.loads(native('.json').read_text())
    if state['message_id'] != 10026 or state['messages_started'] != 9 or state['field_event_running'] or state['controller_stage'] != 9:
        raise RuntimeError('Native conversation did not finish')
    suffixes = ['', '.json', '.audio.json', '.audio.pcm', '.music.bin', '.sound.bin',
                '.camera.bin', '.camera-state.bin', '.movement.bin', '.interaction.bin',
                '.actor.bin', '.actor-9.bin', '.actor-1.bin', '.dialogue.bin', '.choices.bin', '.effects.bin']
    for capture in args.compare:
        same_family(args.capture, capture, suffixes)
    ui = None
    if args.ui:
        same_family(args.capture, args.ui, [s for s in suffixes if s != '.audio.json'])
        audio = json.loads(source(args.ui, '.audio.json').read_text())
        expected = json.loads(native('.audio.json').read_text())
        for key, value in expected.items():
            if not key.startswith('device_') and audio[key] != value:
                raise RuntimeError('UI software audio metadata differs')
        if audio['device_frames_enqueued'] <= 0:
            raise RuntimeError('UI replay submitted no audio')
        ui = {'capture': str(args.ui), 'software_outputs_exact': True,
              'device_frames_enqueued': audio['device_frames_enqueued'],
              'device_underrun_frames': audio['device_underrun_frames'],
              'physical_input_or_listening_verified': False}
    files = [native(s) for s in suffixes]
    files += [source(prefix, s) for prefix in (original, continuation)
              for s in ('.interaction.bin', '.controller.bin', '.actor.bin', '.input', '.log', '.state.zip')]
    files += [Path('config/native-kitchen-talk.directions'),
              args.database]
    report = {'scope': __doc__.strip(), 'elf_sha256': ELF_HASH,
              'reference': str(original), 'continuation': str(continuation), 'native': str(args.capture),
              'native_frame_for_original_callback_zero': origin,
              'selected_npc_index': 0, 'native_alias': -8, 'event_entry': 30, 'message_id': 10026,
              'field_pc_and_movement_gate_exact_frames': 480,
              'hero_root_facing_exact_frames': 450, 'mother_root_facing_exact_frames': 480,
              'root_position_maximum_absolute_error': maximum_position, 'root_position_tolerance': .00002,
              'dialogue_progress_advance_exact_returns': len(after)+len(continued_controller),
              'mother_rendered_script_and_all_channels_exact_frames': len(rendered_mother),
              'diagnostic_window_stage_differences': stage_differences,
              'native_event_end_frame': end[0], 'native_movement_release_frame': released[0],
              'original_movement_release_frame_in_continuation': handoffs[0],
              'original_release_in_native_time': origin+180+handoffs[0],
              'native_movement_after_conversation': True,
              'matching_native_captures': [str(x) for x in args.compare], 'appkit': ui,
              'limitations': ['Diagnostic window construction skips original stages; no full initialization claim.',
                              'Observed skeleton transition timing needs recovery; root comparisons do not validate all bone channels.',
                              'Native initial variable 4:6 is zero; original source save has one. Each restores its prior value.',
                              'Controller SFX, lighting, shadows, full windows, physical input/listening remain incomplete.',
                              'Room transitions, cold startup, battles, saves and complete playthrough remain unfinished.'],
              'sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
    write_exact(args.output, (json.dumps(report, indent=2)+'\n').encode())
    print('Native approach, target/event dispatch, conversation and return to movement verified within stated limits')

if __name__ == '__main__':
    main()
