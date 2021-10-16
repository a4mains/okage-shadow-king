#!/usr/bin/env python3
"""Report field-interval differences without shifting their original time anchor.

This diagnostic retains failures; it does not replace the strict conversation
verifier. Missing original manager/camera records and player payloads are reported,
never interpreted as matching observations or filled with synthetic records.
Agreement covers observed values only; coverage is reported independently.
Rendering, sound and unobserved state are outside this comparison. Optional
target checks cover only idle mode2 frames with already-completed blending; the
original trace's requested target is not an effective target during blending.
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

def indexed_observations(observations, total, name):
    """Keep original labels; absence is allowed, duplicate/reordered labels are not."""
    indexed = {}
    previous = -1
    for row in observations:
        frame = row[0]
        require(isinstance(frame, int) and not isinstance(frame, bool) and
                0 <= frame < total, f'{name} frame outside original interval')
        require(frame > previous, f'{name} frames must be strictly increasing without duplicates')
        indexed[frame] = row
        previous = frame
    return indexed, [frame for frame in range(total) if frame not in indexed]

def compare_observations(*, total, first_frame, managers, movement, dialogue, choices,
                         cameras, camera_states, actors, transitions, camera,
                         controller, original_choices, dispatch_frames=(), unblended_target=False):
    """Compare available channels at one fixed anchor; report gaps separately.

    missing_original_player_frames retains its historical meaning: manager records
    present without a player payload. Player coverage also includes absent managers.
    Coverage describes these traced channels, not rendering, sound, or all game state.
    """
    require(total > 0 and first_frame > 1, 'Invalid interval/preceding native frame')
    require(len(managers) == len(movement) == len(dialogue) == len(choices) == len(cameras) ==
            len(camera_states) == len(actors), 'Native trace extents')
    for name, records in (('manager', managers), ('movement', movement), ('dialogue', dialogue),
                          ('choice', choices), ('camera', cameras), ('camera-state', camera_states),
                          ('actor', actors)):
        require([r[0] for r in records] == list(range(1, len(records) + 1)),
                f'Native {name} frame sequence')
    require(first_frame + total - 1 <= len(managers), 'Native interval incomplete')
    manager_by_frame, missing_manager = indexed_observations(transitions, total, 'Original manager')
    camera_by_frame, missing_camera = indexed_observations(camera, total, 'Original camera')
    require(all(0 <= f < total for f in dispatch_frames), 'Original dispatch frame outside interval')
    require(len(controller) % 2 == 0 and len(controller) == len(original_choices), 'Controller extent')
    differences = {key: [] for key in ('controller', 'choice', 'manager_running_pc_gate',
                   'pad_latch', 'direction_latch', 'player_mask', 'hero_facing_w', 'hero_position',
                   'camera_rotation', 'camera_mode_phase', 'camera_position', 'camera_unblended_target')}
    observed = []
    for before, after, cb, ca in zip(controller[::2], controller[1::2],
                                     original_choices[::2], original_choices[1::2]):
        f = after[0]
        require(before[:2] == cb[:2] == (f, 0x148dc8) and
                after[:2] == ca[:2] == (f, 0x11ff38), 'Controller pairing')
        require(0 <= f < total and (not observed or f > observed[-1]), 'Controller frame order')
        observed.append(f)
        at = first_frame + f - 1
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
    target_frames = []
    missing_player = []
    for r in transitions:
        f = r[0]
        at = first_frame + f - 1
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
    for c in camera:
        f = c[0]
        at = first_frame + f - 1
        r = manager_by_frame.get(f)
        previous_camera = camera_by_frame.get(f - 1)
        if cameras[at][12:16] != c[8:12]:
            differences['camera_rotation'].append(f)
        if (camera_states[at][1], camera_states[at][3]) != c[2:4]:
            differences['camera_mode_phase'].append(f)
        e = error(cameras[at][4:8], c[4:8])
        camera_error = max(camera_error, e)
        if e > 0.00002:
            differences['camera_position'].append(f)
        if (unblended_target and r is not None and previous_camera is not None and c[2] == 2 and
                not r[35] & 256 and not managers[at][4] and f not in dispatch_frames and
                c[3] == previous_camera[3] == 0x3f800000 and
                c[16:24] == previous_camera[16:24] and c[28] == previous_camera[28] and
                camera_states[at][1] == c[2] and
                camera_states[at][3] == camera_states[at - 1][3] == 0x3f800000):
            target_frames.append(f)
            if cameras[at][8:12] != c[12:16]:
                differences['camera_unblended_target'].append(f)
    failed = sorted({f for values in differences.values() for f in values})
    tail = max(failed + missing_player + missing_manager + missing_camera, default=-1) + 1
    missing_by_channel = dict(manager=missing_manager, camera=missing_camera,
                              player=sorted(set(missing_manager + missing_player)))
    coverage = {name: dict(expected_frames=total, observed_frames=total-len(missing))
                for name, missing in missing_by_channel.items()}
    return dict(controller_samples=len(observed),
                unblended_target_check=unblended_target,
                unblended_target_compared_frames=target_frames,
                difference_frames=differences,
                missing_original_manager_frames=missing_manager,
                missing_original_camera_frames=missing_camera,
                missing_original_player_frames=missing_player,
                original_observation_coverage=coverage,
                complete_original_observation_coverage=not any(missing_by_channel.values()),
                hero_position_maximum_error=hero_error, camera_position_maximum_error=camera_error,
                position_tolerance=0.00002, compared_observables_agree=not failed,
                full_timeline_equivalence=False,
                matching_tail=dict(first_original_frame=tail, frames=total-tail,
                                   controller_samples=sum(f >= tail for f in observed)))

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--first-frame', type=int, required=True)
    p.add_argument('--no-dialogue', action='store_true', help='Require empty original controller/choice traces for an interval without dialogue')
    p.add_argument('--unblended-target', action='store_true', help='Compare idle mode2 targets after stable completed blending in both traces')
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
    def original(suffix, width):
        data = path(a.reference, suffix).read_bytes()
        return rows(data, width) if data else []
    managers = native('.interaction.bin', 8)
    movement = native('.movement.bin', 16)
    dialogue = native('.dialogue.bin', 12)
    choices = native('.choices.bin', 12)
    cameras = native('.camera.bin', 16)
    camera_states = native('.camera-state.bin', 5)
    actors = actor_rows(path(a.capture, '.actor-1.bin'))
    transition_observations = original('.transition.bin', 128)
    transitions = [r for r in transition_observations if r[1] == 0x133a98]
    dispatch_frames = {r[0] for r in transition_observations if r[1] == 0x1bb7e0}
    camera = original('.camera.bin', 32)
    controller_bytes = path(a.reference, '.controller.bin').read_bytes()
    choice_bytes = path(a.reference, '.controller.bin.choices.bin').read_bytes()
    if a.no_dialogue:
        require(not controller_bytes and not choice_bytes, 'Unexpected dialogue in no-dialogue interval')
        controller, original_choices = [], []
    else:
        controller = rows(controller_bytes, 64)
        original_choices = rows(choice_bytes, 16)
    compared = compare_observations(
        total=total, first_frame=a.first_frame, managers=managers, movement=movement,
        dialogue=dialogue, choices=choices, cameras=cameras, camera_states=camera_states,
        actors=actors, transitions=transitions, camera=camera, controller=controller,
        original_choices=original_choices, dispatch_frames=dispatch_frames,
        unblended_target=a.unblended_target)
    report = dict(scope=__doc__, elf_sha256=ELF_HASH, native_run=native_run,
                  original_run=reference_run, first_native_frame=a.first_frame,
                  original_frames=total, no_dialogue_interval=a.no_dialogue,
                  **compared,
                  sha256={str(f): digest(f) for f in artifacts})
    write_exact(a.output, (json.dumps(report, indent=2) + '\n').encode())
    print(json.dumps({k: report[k] for k in ('compared_observables_agree', 'matching_tail',
                      'missing_original_manager_frames', 'missing_original_camera_frames',
                      'missing_original_player_frames', 'complete_original_observation_coverage',
                      'hero_position_maximum_error',
                      'camera_position_maximum_error')}))
    print('Difference counts:', {k: len(v) for k, v in compared['difference_frames'].items()})

if __name__ == '__main__':
    main()
