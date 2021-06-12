#!/usr/bin/env python3
"""Report field-interval differences without shifting their original time anchor.

This diagnostic retains failures; it does not replace the strict conversation
verifier. Missing original player payloads are counted, never interpreted as zero.
Rendering, sound, camera targets and unobserved state are outside this comparison.
"""
import argparse
import json
import math
import struct
from pathlib import Path

from prepare_assets import ELF_HASH
from verify_event import digest, write_exact
from verify_interaction_scene import actor_rows
from verify_message_opening import require, rows

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--first-frame', type=int, required=True)
    p.add_argument('--no-dialogue', action='store_true', help='Require empty original controller/choice traces for an interval without dialogue')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    require(a.first_frame > 1, 'Need preceding completed native frame')
    artifacts = []

    def path(prefix, suffix):
        result = Path(str(prefix) + suffix)
        artifacts.append(result)
        return result

    reference_run = json.loads(path(a.reference, '.run.json').read_text())
    native_run = json.loads(path(a.capture.with_suffix(''), '.run.json').read_text())
    require(reference_run['exit_code'] == native_run['exit_code'] == 0, 'Incomplete capture')
    total = int(reference_run['command'][2])
    log = path(a.reference, '.log').read_text()
    source = reference_run['command'][3]
    require(f'Loaded reference state {source}' in log and
            f'Stopped: vblanks={total} exit_request=0' in log, 'Original completion/continuity')
    artifacts.append(Path(source))
    native = lambda suffix, width: rows(path(a.capture, suffix).read_bytes(), width)
    original = lambda suffix, width: rows(path(a.reference, suffix).read_bytes(), width)
    managers = native('.interaction.bin', 8)
    movement = native('.movement.bin', 16)
    dialogue = native('.dialogue.bin', 12)
    choices = native('.choices.bin', 12)
    cameras = native('.camera.bin', 16)
    camera_states = native('.camera-state.bin', 5)
    actors = actor_rows(path(a.capture, '.actor-1.bin'))
    require(len(managers) == len(movement) == len(dialogue) == len(choices) == len(cameras) ==
            len(camera_states) == len(actors), 'Native trace extents')
    require([r[0] for r in managers] == list(range(1, len(managers) + 1)), 'Native frame sequence')
    require(a.first_frame + total - 1 <= len(managers), 'Native interval incomplete')
    transitions = [r for r in original('.transition.bin', 128) if r[1] == 0x133a98]
    camera = original('.camera.bin', 32)
    controller_bytes = path(a.reference, '.controller.bin').read_bytes()
    choice_bytes = path(a.reference, '.controller.bin.choices.bin').read_bytes()
    if a.no_dialogue:
        require(not controller_bytes and not choice_bytes, 'Unexpected dialogue in no-dialogue interval')
        controller, original_choices = [], []
    else:
        controller = rows(controller_bytes, 64)
        original_choices = rows(choice_bytes, 16)
    require([r[0] for r in transitions] == [r[0] for r in camera] == list(range(total)),
            'Original manager/camera extent')
    require(len(controller) % 2 == 0 and len(controller) == len(original_choices), 'Controller extent')
    differences = {key: [] for key in ('controller', 'choice', 'manager_running_pc_gate',
                   'pad_latch', 'direction_latch', 'player_mask', 'hero_facing_w', 'hero_position',
                   'camera_rotation', 'camera_mode_phase', 'camera_position')}
    observed = []
    for before, after, cb, ca in zip(controller[::2], controller[1::2],
                                     original_choices[::2], original_choices[1::2]):
        f = after[0]
        require(before[:2] == cb[:2] == (f, 0x148dc8) and
                after[:2] == ca[:2] == (f, 0x11ff38), 'Controller pairing')
        require(0 <= f < total and (not observed or f > observed[-1]), 'Controller frame order')
        observed.append(f)
        at = a.first_frame + f - 1
        expected = tuple(after[i] for i in (32, 33, 34, 35, 36, 16, 21, 17, 7, 43))
        if dialogue[at][2:] != expected:
            differences['controller'].append(f)
        if choices[at][2:6] != ca[2:6]:
            differences['choice'].append(f)
        require(managers[at][1] == after[9], 'Controller/manager pad disagreement')

    def error(words_a, words_b):
        decode = lambda w: struct.unpack('<' + 'f' * len(w), struct.pack('<' + 'I' * len(w), *w))
        va, vb = decode(words_a), decode(words_b)
        require(all(math.isfinite(v) for v in va + vb), 'Nonfinite position')
        return max(abs(x - y) for x, y in zip(va, vb))

    hero_error = camera_error = 0.
    missing_player = []
    for r, c in zip(transitions, camera):
        f = r[0]
        at = a.first_frame + f - 1
        prior = managers[at - 1]
        if (prior[4], prior[7], prior[6]) != (int(bool(r[35] & 256)), r[36], r[118]):
            differences['manager_running_pc_gate'].append(f)
        if managers[at][1] != r[33] >> 16:
            differences['pad_latch'].append(f)
        if movement[at][1] & 0xf000 != r[33] & 0xf000:
            differences['direction_latch'].append(f)
        if r[28]:
            if prior[5] != r[48]:
                differences['player_mask'].append(f)
            hero = actors[at - 1][8:16]
            if hero[:4] != r[40:44] or hero[7] != r[47]:
                differences['hero_facing_w'].append(f)
            e = error(hero[4:7], r[44:47])
            hero_error = max(hero_error, e)
            if e > 0.00002:
                differences['hero_position'].append(f)
        else:
            missing_player.append(f)
        if cameras[at][12:16] != c[8:12]:
            differences['camera_rotation'].append(f)
        if (camera_states[at][1], camera_states[at][3]) != c[2:4]:
            differences['camera_mode_phase'].append(f)
        e = error(cameras[at][4:8], c[4:8])
        camera_error = max(camera_error, e)
        if e > 0.00002:
            differences['camera_position'].append(f)
    failed = sorted({f for values in differences.values() for f in values})
    tail = max(failed + missing_player, default=-1) + 1
    report = dict(scope=__doc__, elf_sha256=ELF_HASH, native_run=native_run,
                  original_run=reference_run, first_native_frame=a.first_frame,
                  original_frames=total, controller_samples=len(observed),
                  no_dialogue_interval=a.no_dialogue,
                  difference_frames=differences, missing_original_player_frames=missing_player,
                  hero_position_maximum_error=hero_error, camera_position_maximum_error=camera_error,
                  position_tolerance=0.00002, compared_observables_agree=not failed,
                  full_timeline_equivalence=False,
                  matching_tail=dict(first_original_frame=tail, frames=total-tail,
                                     controller_samples=sum(f >= tail for f in observed)),
                  sha256={str(f): digest(f) for f in artifacts})
    write_exact(a.output, (json.dumps(report, indent=2) + '\n').encode())
    print(json.dumps({k: report[k] for k in ('compared_observables_agree', 'matching_tail',
                      'missing_original_player_frames', 'hero_position_maximum_error',
                      'camera_position_maximum_error')}))
    print('Difference counts:', {k: len(v) for k, v in differences.items()})

if __name__ == '__main__':
    main()
