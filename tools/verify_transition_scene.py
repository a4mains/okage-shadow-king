#!/usr/bin/env python3
"""Audit the native kitchen exit and arrival; excludes complete loading/presentation fidelity."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from prepare_assets import ELF_HASH
from verify_choice_scene import rows, same_family
from verify_effect_scene import effect_rows
from verify_interaction_scene import actor_rows
from verify_movement_scene import error
from verify_event import write_exact

def require(value, message):
    if not value:
        raise RuntimeError(message)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, default=Path('local/native-kitchen-exit-5200-debug-v4.png'))
    parser.add_argument('--compare', action='append', type=Path, default=[])
    parser.add_argument('--ui', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    source = lambda prefix, suffix: Path(str(prefix)+suffix)
    native = lambda suffix: source(args.capture, suffix)
    original = Path('local/reference-kitchen-exit-180-v1')
    require('Stopped: vblanks=180 exit_request=0' in source(original, '.log').read_text(), 'Incomplete original exit')
    require(hashlib.sha256(Path('local/extracted/SCUS_971.29').read_bytes()).hexdigest() == ELF_HASH, 'Wrong ELF')
    observed = rows(source(original, '.transition.bin'), 128)
    before = [r for r in observed if r[1] == 0x133a98 and r[0] <= 63]
    require([r[0] for r in before] == list(range(64)), 'Missing exit field observations')
    movement = rows(native('.movement.bin'), 16)
    cameras = rows(native('.camera.bin'), 16)
    transitions = rows(native('.transition.bin'), 8)
    interactions = rows(native('.interaction.bin'), 8)
    hero = actor_rows(native('.actor-1.bin'))
    effects = effect_rows(native('.effects.bin'))
    require(all(len(x) == 5200 for x in [movement, cameras, transitions, interactions, hero, effects]), 'Incomplete native route')

    origin = 5050
    maximum_position = 0.
    bubble_frames = 0
    ram_path = Path('local/research/kitchen-end700-v2-ee.bin')
    ram = ram_path.read_bytes()
    word = lambda at: struct.unpack_from('<I', ram, at)[0]
    table = word(0x24e780)
    texture_map = {word(table+4*i): i for i in range(word(0x1f8498))}
    for r in before:
        i = origin+r[0]-2
        require(cameras[i][1] == r[36], 'Field event PC mismatch')
        for native_word, original_word in [(2,48), (4,49), (5,50), (6,51), (7,52), (8,53), (9,54), (10,55)]:
            require(movement[i][native_word] == r[original_word], f'Player contact/state mismatch at {r[0]}')
        require(hero[i][8:12] == r[40:44], 'HERO facing mismatch')
        maximum_position = max(maximum_position, error(hero[i][12:16], r[44:48]))
        require(bool(movement[i][15]) == bool(r[29]), 'Bubble lifetime mismatch')
        require((movement[i+1][1]&0xf000) == (r[33]&0xf000), 'Observed input latch mismatch')
        if r[29]:
            bubble_frames += 1
            require(r[30] == 1 and len(effects[i][2]) == 1, 'Missing bubble payload')
            e, = effects[i][2]
            require(e[1] == 0xffffffff and e[2:4] == (r[32], r[31]), 'Bubble alias/phase/flags mismatch')
            require(e[4:8] == r[64:68], 'Bubble attachment position mismatch')
            require(r[80:84] == (0,0,0,0), 'Original symbol parameter is not zero')
            for offset, at in [(16,84), (27,88)]:
                require(e[offset] == texture_map[r[at]&0xffff], 'Bubble logical texture mismatch')
                require(e[offset+1:offset+3] == r[at+1:at+3], 'Bubble UV mismatch')
    require(maximum_position <= .00002, 'HERO position exceeds established host-math tolerance')
    starts = [(r[0],r[3]) for r in observed if r[1] == 0x1bb7e0]
    require(starts == [(60,1),(140,50)], 'Unexpected original event route')
    requested = [r for r in transitions if r[3] != 0xffffffff]
    require(requested == [(5110,72,0,70,2,2,0,40), (5111,72,0,70,2,1,0,40),
                          (5112,72,0,70,2,0,0,40), (5113,72,0,70,2,0,1,40)], 'Native transition/countdown differs')
    request, = [r for r in observed if r[1] == 0x124930]
    require(request[0] == 63 and request[4:7] == (2,70,2), 'Original scene request differs')
    require(interactions[5109][1:6] == (0x40,0,1,0,0), 'Exit was not immediate input-driven event1')

    arrival = next(r for r in observed if r[1] == 0x133a98 and r[0] == 141)
    require(arrival[21:24:2] == (70,2) and arrival[28] == 1, 'Missing original destination player')
    require(hero[5112][8:16] == arrival[40:48], 'STG-derived entrance differs from original arrival')
    require(cameras[5112][1] == arrival[36] == 0x1b0, 'Startup event did not finish at the observed entry')
    require(all(t[1:3] == (70,2) for t in transitions[5113:]), 'Destination did not remain active')
    require(hero[5158][12:16] != hero[5175][12:16], 'Movement did not resume after arrival')
    require(all(m[2] == 0 for m in movement[5112:]), 'Destination movement remains gated')
    variables = native('.variables.bin').read_bytes()
    require(len(variables) == 0x1000 and variables[0x110]&1, 'Kitchen progression bit was lost')
    require(not any(variables[0x10:0x110]), 'Temporary field bits were not reset')
    require(struct.unpack_from('<2I', variables, 0x510) == (70,2), 'Scene/entrance variables differ')
    require(struct.unpack_from('<I', variables, 0x528)[0] == 1, 'Field input gate initialization differs')

    order_prefix = Path('local/reference-contact-order-45-v2')
    require('Stopped: vblanks=45 exit_request=0' in source(order_prefix, '.log').read_text(), 'Incomplete order capture')
    order = rows(source(order_prefix, '.transition.bin'), 128)
    for frame in range(24,45):
        require([r[1] for r in order if r[0] == frame] == [0x133a98,0x101118,0x134c18], 'Contact scheduler order changed')

    family = ['', '.json', '.audio.pcm', '.music.bin', '.sound.bin', '.camera.bin', '.camera-state.bin',
              '.movement.bin', '.interaction.bin', '.transition.bin', '.variables.bin', '.effects.bin',
              '.actor.bin', '.actor-9.bin', '.actor-1.bin', '.dialogue.bin', '.choices.bin']
    for compare in args.compare:
        same_family(args.capture, compare, family+['.audio.json'])
    ui = None
    if args.ui:
        same_family(args.capture, args.ui, family)
        ui = json.loads(source(args.ui, '.audio.json').read_text())
        require(ui['frames'] == 5200*800, 'AppKit synthesized duration mismatch')
        pending = ui['frames']-ui['device_frames_enqueued']
        require(0 <= pending <= 16384 and ui['device_frames_enqueued'] > 0,
                'AppKit audio counters exceed the producer-ring contract')
        ui['producer_frames_pending_at_capture'] = pending
    database = Path('local/xpf-decoded/FLDDATA/FLDSET.XDB')
    require(hashlib.sha256(database.read_bytes()).hexdigest() == 'e1085fc18788d42b10b11d6fdbf8e8bcb3de67ef11003669b4de9e4b30d1f0ba', 'Wrong runtime field database')
    files = [args.capture, native('.transition.bin'), native('.variables.bin'), source(original, '.transition.bin'),
             source(order_prefix, '.transition.bin'), ram_path, database, Path('config/native-kitchen-exit.directions')]
    report = {'scope': __doc__.strip(), 'elf_sha256': ELF_HASH, 'native': str(args.capture),
              'reference': str(original), 'exact_preexit_field_updates': 64,
              'root_position_maximum_absolute_error': maximum_position, 'root_position_tolerance': .00002,
              'exact_bubble_phase_flags_attachment_texture_uv_frames': bubble_frames,
              'contact_job_order_observed_frames': 21, 'input_event': 1, 'destination_scene': 70, 'entrance': 2,
              'request_frame': 5110, 'native_load_frame': 5113, 'original_startup_frame': 140,
              'arrival_root_transform_exact': True, 'startup_event': 50, 'native_movement_after_arrival': True,
              'matching_captures': [str(x) for x in args.compare], 'appkit': ui,
              'limits': ['Synchronous native loading does not reproduce original loading latency or cold initialization.',
                         'No full destination skeleton, room lighting, shadows or transition-pixel equivalence claim.',
                         'Blocked progression message10030 uses diagnostic window initialization; this route does not exercise it.',
                         'Post-arrival movement is demonstrated natively, not yet paired with original movement in this room.',
                         'Original capture/raw RAM are auditor-only references; native launch reads owned asset imports.',
                         'Physical controller/listening, sustained performance, battles, saves and full playthrough remain open.'],
              'sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
    write_exact(args.output, (json.dumps(report, indent=2)+'\n').encode())
    print('Native exit dispatch/countdown, bubble lifecycle, original arrival and native resumed movement verified')

if __name__ == '__main__':
    main()
