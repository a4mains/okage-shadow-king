#!/usr/bin/env python3
"""Compare the native scripted HERO entrance through the pending audio service."""
import argparse
import json
from pathlib import Path

from verify_animation import sha
from verify_imo import rows, run

def app_run(app, frames, presses, capture):
    run(app, 'local/font-input/CMNDATA/FONT', 'local/text-input/FLDDATA/MES/FIELD.PME',
        '--scene', 'local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF/a.xmp',
        '--event', 'local/xpf-decoded/FLDDATA/EVENT/EVDATA.XPF/e_home1c.xmt',
        '--actor', 'local/xpf-decoded/FLDDATA/OBJ/MAM.XPF/a.xmd',
        '--actor-bind', '-9=local/xpf-decoded/FLDDATA/OBJ/IMO.XPF/a.xmd',
        '--actor-bind', '-1=local/xpf-decoded/FLDDATA/OBJ/HERO.XPF/a.xmd',
        '--frames', frames, '--press', ','.join(map(str, presses)), '--capture', capture)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=Path('build/native'))
    p.add_argument('--output', type=Path, default=Path('local/player-validation'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    app = args.build/'okage-native.app/Contents/MacOS/okage-native'
    reference = Path('local/reference-player-1460-v1.bin')
    controller = Path('local/reference-player-controller-1460-v1.bin')
    retail = rows(reference, 920)
    controls = rows(controller, 64)
    if not retail or retail[-1][0] != 319 or [r[0] for r in retail] != list(range(retail[0][0], 320)):
        raise ValueError('Incomplete/discontinuous player capture')
    if not controls or len(controls) % 2:
        raise ValueError('Incomplete controller capture')
    for before, after in zip(controls[::2], controls[1::2]):
        if before[:2] != (after[0], 0x148dc8) or after[1] != 0x11ff38:
            raise ValueError('Unpaired controller capture')
        if before[8] not in (0, 0x40) or before[9] not in (0, 0x40):
            raise ValueError('Input outside entrance replay scope')
    normal = args.output/'frame-1100.png'
    app_run(app, 1100, range(100, 1100, 40), normal)
    normal_actors = rows(Path(str(normal)+'.actor-1.bin'), 946)
    normal_scene = rows(Path(str(normal)+'.camera.bin'), 16)
    state = json.loads(Path(str(normal)+'.json').read_text())
    if state['messages_started'] != 2 or state['message_id'] != 10101 or state['text_progress'] != 5:
        raise ValueError('Second message did not finish')
    if normal_scene[-1][1:3] != (0xda8, 0x45):
        raise ValueError('Expected explicit pending music/sound-bank service')
    visible = next(r[0] for r in normal_actors if r[7])
    if any(r[7] for r in normal_actors[:visible-1]):
        raise ValueError('Player visible before entrance')
    offset = visible - retail[0][0]
    boundary = next(i for i, r in enumerate(retail) if r[1] not in (0xd5c, 0xd60))
    samples = retail[:boundary]
    if not samples or samples[0][1] != 0xd5c or samples[-1][1] != 0xd60:
        raise ValueError('Unexpected walk/idle/close-wait interval')
    if controls[-1][0] < retail[boundary][0]:
        raise ValueError('Controller capture does not cover entrance')
    observed = [r[0] for r in controls[::2] if r[0] >= retail[0][0] and r[9] & 0x40]
    presses = list(range(100, visible, 40)) + [frame+offset for frame in observed]
    aligned = args.output/'aligned.png'
    app_run(app, retail[boundary][0]+offset, presses, aligned)
    native = rows(Path(str(aligned)+'.actor-1.bin'), 946)
    scene = rows(Path(str(aligned)+'.camera.bin'), 16)
    for r in samples:
        n = native[r[0]+offset-1]
        if r[5] != 228 or n[5:8] != (228, 26, 1):
            raise ValueError('Player channel/texture/visibility layout mismatch')
        if r[1:4] != n[1:4] or r[8:20] != n[8:20]:
            raise ValueError('Player event/script/root mismatch at frame %d' % r[0])
        if r[8:] != n[8:920]:
            raise ValueError('Player channels differ at frame %d' % r[0])
    final = native[-1]
    if scene[-1][1:3] != (0xda8, 0x45):
        raise ValueError('Native continuation failed to reach the audio boundary on the same frame')
    if final[2:4] != retail[boundary][2:4] or final[8:920] != retail[boundary][8:]:
        raise ValueError('Player transform/animation differs at the audio boundary')
    report = {
        'scope': 'Scripted HERO entrance and close-up with optional audio disabled; diagnostic startup, not player control',
        'elf_sha256': sha(Path('local/extracted/SCUS_971.29')),
        'native_app_sha256': sha(app),
        'reference_sha256': sha(reference), 'controller_sha256': sha(controller),
        'native_state': state,
        'reference_visible_frame': retail[0][0], 'native_visible_frame': visible,
        'native_frame_offset': offset, 'observed_pressed_frames': observed,
        'event_script_root_exact_frames': len(samples),
        'all_912_channel_words_exact_from_reference_frame': samples[0][0],
        'channel_exact_frames': len(samples), 'full_observed_channel_equivalence': True,
        'audio_boundary': {'reference_frame': retail[boundary][0],
                           'original_pc': hex(retail[boundary][1]), 'native_pc': '0xda8',
                           'native_pending_opcode': '0x45', 'actor_channels_and_script_exact': True},
        'native_trace_sha256': sha(Path(str(aligned)+'.actor-1.bin')),
        'limits': ['Explicit assets/aliases and diagnostic window/idle initialization',
                   'No player controls, collision, shadows, effects or faithful lighting; optional audio disabled in this comparison',
                   'Original proceeds into audio services beyond the bounded matched event interval',
                   'Full-game observer does not capture texture selection; prior HERO clip tests cover isolated motions'],
    }
    (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
