#!/usr/bin/env python3
"""Compare nighttime startup at observed dispatch; retain fidelity failures.

This compares lifecycle phases, not original loading duration. It does not
validate pixels, audio, camera targets or post-load actor animation phase.
"""
import argparse
import json
from pathlib import Path

from prepare_assets import ELF_HASH
from verify_event import digest, write_exact
from verify_interaction_scene import actor_rows
from verify_message_opening import require, rows

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    original = Path('local/reference-home1a-night-outdoor-360-v1')
    artifacts = []

    def path(prefix, suffix):
        p = Path(str(prefix) + suffix)
        artifacts.append(p)
        return p

    ref_run = json.loads(path(original, '.run.json').read_text())
    native_run = json.loads(path(args.capture.with_suffix(''), '.run.json').read_text())
    require(ref_run['exit_code'] == native_run['exit_code'] == 0, 'Incomplete runs')
    require('local/reference-home1a-night-door-240-v1.state.zip' in ref_run['command'],
            'Wrong original continuation')
    observed = rows(path(original, '.transition.bin').read_bytes(), 128)
    cameras = rows(path(original, '.camera.bin').read_bytes(), 32)
    managers = rows(path(args.capture, '.interaction.bin').read_bytes(), 8)
    native_cameras = rows(path(args.capture, '.camera.bin').read_bytes(), 16)
    states = rows(path(args.capture, '.camera-state.bin').read_bytes(), 5)
    transitions = rows(path(args.capture, '.transition.bin').read_bytes(), 8)
    actors = actor_rows(path(args.capture, '.actor-1.bin'))
    require(len(managers) == len(native_cameras) == len(states) == len(transitions) == len(actors),
            'Native extents differ')
    require([r[0] for r in managers] == list(range(1, len(managers) + 1)), 'Native frame order')
    dispatch, = [r for r in observed if r[1] == 0x1bb7e0]
    require(dispatch[0] == 83 and dispatch[3] == 60, 'Original startup dispatch changed')
    require(transitions[21658][3:7] == (1, 1, 0, 1), 'Native destination dispatch changed')
    require(managers[21658][4:8] == (1, 1, 1, 0x7acc), 'Native first startup result')
    # Boundaries follow actual startup dispatch, never a fitted agreement offset.
    native_first, original_first, samples = 21659, 83, 202
    require(len(managers) >= native_first + samples - 1, 'Incomplete native interval')
    before = {r[0]: r for r in observed if r[1] == 0x133a98}
    camera = {r[0]: r for r in cameras}
    differences = {k: [] for k in ('manager', 'hero_root', 'camera_position',
                                  'camera_rotation', 'camera_mode_phase')}
    first_rotation_difference = None
    for offset in range(samples):
        f, n = original_first + offset, native_first + offset - 1
        r, c, m = before[f + 1], camera[f], managers[n]
        require(r[28] == 1, 'Missing original HERO')
        checks = {
            'manager': (m[4], m[7], m[5], m[6]) ==
                       (int(bool(r[35] & 256)), r[36], r[48], r[118]),
            'hero_root': actors[n][8:16] == r[40:48],
            'camera_position': native_cameras[n][4:8] == c[4:8],
            'camera_rotation': native_cameras[n][12:16] == c[8:12],
            'camera_mode_phase': (states[n][1], states[n][3]) == c[2:4],
        }
        for key, equal in checks.items():
            if not equal:
                differences[key].append(f + 1 if key in ('manager', 'hero_root') else f)
        if not checks['camera_rotation'] and first_rotation_difference is None:
            first_rotation_difference = dict(original_camera_frame=f, native_frame=n + 1,
                                            original_words=c[8:12],
                                            native_words=native_cameras[n][12:16])
    report = dict(scope=__doc__, elf_sha256=ELF_HASH, original_run=ref_run,
                  native_run=native_run, native_startup_frame=native_first,
                  original_startup_frame=original_first, compared_samples=samples,
                  original_manager_first=84, original_camera_first=83,
                  difference_frames=differences,
                  first_rotation_difference=first_rotation_difference,
                  all_compared_words_exact=not any(differences.values()),
                  full_loading_timeline_equivalence=False,
                  sha256={str(p): digest(p) for p in artifacts})
    write_exact(args.output, (json.dumps(report, indent=2) + '\n').encode())
    print('Difference counts:', {k: len(v) for k, v in differences.items()})

if __name__ == '__main__':
    main()
