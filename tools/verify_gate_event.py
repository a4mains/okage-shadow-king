#!/usr/bin/env python3
"""Audit the native field gate event against two completed original captures.

Checks 440 manager samples and 186 scripted camera samples. Exploration camera
trajectories, complete movement, gate bone animation, sound and pixels are not
covered by these exact comparisons. No reference data is an application input.
"""
import argparse
import json
from pathlib import Path
import struct
from verify_event import write_exact
from verify_message_opening import require, digest
from verify_interaction_scene import actor_rows

REFERENCES = [
    ('reference-field-gate-240-v1', 7521, 240, 186, 240,
     '2b4a5c76f9e7be1854a307ad34cb5a7b4a61a0a6c2fbec5c0f61335d5be636d6',
     '42904237f4a542b37fde1bce4d0e9cac1cf5c94bc52f9aad9758d0e3a379ba77'),
    ('reference-gate-opening-200-v1', 7761, 200, 0, 132,
     '775912024ef359681ca272c6aff3a1857a4756c92947c5509c43238d24591651',
     'b9d53d3b5eca33f8c45d6cc5b408eaba49282e763341adbf71eeefd9d62ea77a')]

def rows(path, width):
    return list(struct.iter_unpack('<' + str(width) + 'I', path.read_bytes()))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--prior-capture',type=Path,default=Path('local/native-field-stairs-7520-sky-v2.png'),
                        help='Independent7520-frame replay for prefix consistency; not original-game evidence')
    args = parser.parse_args()
    base = str(args.capture)
    path = lambda suffix: Path(base + suffix)
    manifest = Path(base.removesuffix('.png') + '.run.json')
    run = json.loads(manifest.read_text())
    require(run['exit_code'] == 0, 'Incomplete native gate capture')
    native = rows(path('.interaction.bin'), 8)
    camera = rows(path('.camera.bin'), 16)
    hero = actor_rows(path('.actor-1.bin'))
    require(len(native) == len(camera) == len(hero) and len(native) >= 7960,
            'Native gate trace extent')
    for source in [native, camera, hero]:
        require(all(r[0] == i + 1 for i, r in enumerate(source)), 'Native frame identity')
    files = [args.capture, manifest, path('.interaction.bin'), path('.camera.bin'), path('.actor-1.bin')]
    reports = []
    for name, anchor, count, first, end, interaction_hash, camera_hash in REFERENCES:
        prefix = 'local/' + name
        interaction = Path(prefix + '.interaction.bin')
        original_camera = Path(prefix + '.camera.bin')
        original_manifest = Path(prefix + '.run.json')
        original_run = json.loads(original_manifest.read_text())
        require(original_run['exit_code'] == 0, 'Incomplete original gate capture')
        require(digest(interaction) == interaction_hash and digest(original_camera) == camera_hash,
                'Changed original gate traces')
        source = rows(interaction, 176)
        manager = [r for r in source if r[1] == 0x133a98]
        require([r[0] for r in manager] == list(range(count)), 'Original manager extent')
        for r in manager:
            # Original manager entry precedes the tick; native rows follow it.
            n = native[anchor + r[0] - 2]
            require((n[4], n[7], n[5], n[6]) ==
                    (int(bool(r[20] & 0x100)), r[21], r[15], r[150]),
                    f'Manager differs at {name}:{r[0]}')
        source_camera = rows(original_camera, 32)
        require([r[0] for r in source_camera] == list(range(count)), 'Original camera extent')
        for r in source_camera[first:end]:
            require(camera[anchor + r[0] - 1][4:] == r[4:8] + r[12:16] + r[8:12],
                    f'Scripted camera differs at {name}:{r[0]}')
        if count == 240:
            require([(r[0], r[3]) for r in source if r[1] == 0x1bb7e0] == [(186, 4)],
                    'Original gate dispatch changed')
            require(native[7706][3:6] == (4, 1, 1), 'Native gate dispatch changed')
        else:
            require([(r[0], r[9], r[15]) for r in source if r[1] == 0x132ca8] == [(131, 5, 1)],
                    'Original gate handoff changed')
            require(hero[7959][12:16] == manager[-1][28:32], 'Gate HERO endpoint differs')
        files.extend([interaction, original_camera, original_manifest])
        reports.append(dict(reference=name,original_run=original_run,first_native_frame=anchor,
                            manager_words=count*4,camera_words=(end-first)*12,
                            first_camera_frame=first,end_camera_frame_exclusive=end))
    require(native[7890][4:7] == (0, 1, 1) and native[7891][4:7] == (0, 0, 1),
            'Gate completion/control handoff differs')
    before = str(args.prior_capture)
    for suffix in ['.dialogue.bin', '.choices.bin', '.interaction.bin', '.movement.bin', '.camera.bin', '.actor-1.bin']:
        previous = Path(before + suffix)
        if suffix=='.interaction.bin':require(len(rows(previous,8))==7520,'Prior capture extent')
        require(path(suffix).read_bytes().startswith(previous.read_bytes()), 'Prior route changed: ' + suffix)
        files.extend([previous, path(suffix)])
    report = dict(scope=__doc__, native_run=run, intervals=reports, manager_words=1760,
                  scripted_camera_words=2232,hero_endpoint_words=4,
                  event_frame=7707,script_finished_frame=7891,control_return_frame=7892,
                  unchanged_prefix_frames=7520,prefix_capture=before,
                  prefix_scope='Native replay consistency against the selected capture; original comparisons are separate.',
                  sha256={str(p):digest(p) for p in files})
    write_exact(args.output,(json.dumps(report,indent=2)+'\n').encode())
    print('440 manager samples,186 scripted camera samples and HERO endpoint match')

if __name__ == '__main__':
    main()
