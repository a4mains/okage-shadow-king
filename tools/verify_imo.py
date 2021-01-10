#!/usr/bin/env python3
"""Validate the second native opening actor, names and input-aligned scene segment."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from verify_animation import compare, sha

def run(*args):
    subprocess.run([str(a) for a in args], check=True)

def app_run(app, frames, presses, capture):
    run(app, 'local/font-input/CMNDATA/FONT', 'local/text-input/FLDDATA/MES/FIELD.PME',
        '--scene', 'local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF/a.xmp',
        '--event', 'local/xpf-decoded/FLDDATA/EVENT/EVDATA.XPF/e_home1c.xmt',
        '--actor', 'local/xpf-decoded/FLDDATA/OBJ/MAM.XPF/a.xmd',
        '--actor-bind', '-9=local/xpf-decoded/FLDDATA/OBJ/IMO.XPF/a.xmd',
        '--frames', frames, '--press', ','.join(map(str, presses)), '--capture', capture)

def rows(path, width):
    return list(struct.iter_unpack('<%dI' % width, path.read_bytes()))

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=Path('build/native'))
    p.add_argument('--output', type=Path, default=Path('local/imo-validation-final'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    native = args.build/'okage-assets'
    app = args.build/'okage-native.app/Contents/MacOS/okage-native'
    base = Path('local/xpf-decoded/FLDDATA/OBJ/IMO.XPF')
    refs = Path('local/imo-validation-initial')
    clips, names = [], []
    for entry in (1, 2, 3):
        out = args.output/('imo-%d.native.bin' % entry)
        reference = refs/('imo-%d.retail.bin' % entry)
        run(native, 'animation', base/'a.xmt', 174, entry, 180, out, base/'a.xmd', base/'a.xta')
        result = compare(reference, out, 174, 85, 180, 22)
        if not result['passed']:
            raise ValueError('Original IMO clip mismatch')
        clips.append(dict(result, entry=entry, native_sha256=sha(out), reference_sha256=sha(reference)))
    for i, name in enumerate(('Ari', 'Stan', 'A B', 'ABCDEFGHIJKL')):
        out = args.output/('name-%d.native.bin' % i)
        reference = refs/('name-%d.retail.bin' % i)
        run(native, 'pme-name', 'local/text-input/FLDDATA/MES/FIELD.PME', 10101, name, out)
        if out.read_bytes() != reference.read_bytes():
            raise ValueError('Original player-name binding mismatch')
        names.append(dict(name=name, bytes_exact=True, sha256=sha(out)))
    normal = args.output/'frame-1100.png'
    app_run(app, 1100, range(100, 1100, 40), normal)
    state = json.loads(Path(str(normal)+'.json').read_text())
    scene = rows(Path(str(normal)+'.camera.bin'), 16)
    if state['message_id'] != 10101 or state['messages_started'] != 2 or state['text_progress'] != 5:
        raise ValueError('Native second message did not complete')
    if scene[-1][1:3] != (0xd24, 7):
        raise ValueError('Unexpected player-entrance continuation')
    actor = rows(Path(str(normal)+'.actor-9.bin'), 726)
    visible = next(r[0] for r in actor if r[7])
    if visible != state['close_wait_release_frame'] or any(r[7] for r in actor[:visible-1]):
        raise ValueError('Actor visibility did not follow the event')
    reference = Path('local/reference-imo-controller-1140-v2.bin')
    control = Path('local/reference-imo-controller-input-1140-v2.bin')
    retail = rows(reference, 704)
    controls = rows(control, 64)
    if len(retail) != 196 or [r[0] for r in retail] != list(range(124, 320)):
        raise ValueError('Incomplete original IMO capture')
    if len(controls) % 2 or not controls or controls[-1][0] != 319:
        raise ValueError('Incomplete simultaneous controller capture')
    for before, after in zip(controls[::2], controls[1::2]):
        if before[:2] != (after[0], 0x148dc8) or after[1] != 0x11ff38 or before[8] not in (0, 0x40) or before[9] not in (0, 0x40):
            raise ValueError('Controller capture outside replay scope')
    offset = visible - retail[0][0]
    presses = list(range(100, visible, 40))
    observed = [r[0] for r in controls[::2] if r[0] >= retail[0][0] and r[9] & 0x40]
    presses += [frame+offset for frame in observed]
    aligned = args.output/'aligned.png'
    app_run(app, retail[-1][0]+offset, presses, aligned)
    observed_native = rows(Path(str(aligned)+'.actor-9.bin'), 726)
    for r in retail:
        n = observed_native[r[0]+offset-1]
        if r[5] != 174 or n[5:8] != (174, 22, 1):
            raise ValueError('Actor layout/visibility mismatch')
        if r[1:4] != n[1:4] or r[8:20] != n[8:20]:
            raise ValueError('Original IMO event/script/root mismatch at frame %d' % r[0])
        if r[8:] != n[8:704]:
            raise ValueError('Original IMO channels differ at frame %d' % r[0])
    report = {
        'scope': "Two-actor HOME1C segment and second message; diagnostic initial actor/window state, not cold boot or playable game",
        'elf_sha256': sha(Path('local/extracted/SCUS_971.29')),
        'native_tool_sha256': sha(native), 'native_app_sha256': sha(app),
        'actor_assets': {name: sha(base/name) for name in ('a.xmd', 'a.xmt', 'a.xta')},
        'clips': clips, 'names': names, 'native_state': state,
        'native_final_pc': '0xd24', 'pending_actor': -1, 'pending_opcode': 7,
        'reference_sha256': sha(reference), 'controller_sha256': sha(control),
        'integration': {'exact_event_script_root_frames': len(retail), 'root_words_per_frame': 12,
                        'reference_first_visible_frame': retail[0][0], 'native_first_visible_frame': visible,
                        'native_frame_offset': offset, 'observed_pressed_frames': observed,
                        'all_696_channel_words_exact_from_reference_frame': retail[0][0],
                        'exact_channel_frames': len(retail),
                        'full_channel_trace_equivalent': True,
                        'native_trace_sha256': sha(Path(str(aligned)+'.actor-9.bin'))},
        'limits': ['Hidden channel advancement is corrected; this does not prove cold actor initialization',
                   'Actor assets/aliases and ready window layers remain explicit diagnostic bindings',
                   'Message state resets and names are native; full constructor/presentation/choice behavior remains open',
                   'Full-scene trace does not observe texture bindings; isolated clips do',
                   'No native player control, audio, shadows, effects, battles, scene transitions or saves'],
    }
    (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report['integration'], indent=2))

if __name__ == '__main__':
    main()
