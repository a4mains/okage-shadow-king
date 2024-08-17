#!/usr/bin/env python3
"""Report field-interval differences without shifting their original time anchor.

This diagnostic retains failures; it does not replace the strict conversation
verifier. Missing original manager/camera records and player payloads are reported,
never interpreted as matching observations or filled with synthetic records.
Agreement covers observed values only; coverage is reported independently.
Rendering, sound and unobserved state are outside this comparison. Optional
target checks cover only idle mode2 frames with already-completed blending; the
original trace's requested target is not an effective target during blending.
Opt-in sparse native indexing retains absolute row labels across battle gaps;
missing rows and prior-frame dependencies remain explicit, without reanchoring.
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
                         controller, original_choices, dispatch_frames=(), unblended_target=False,
                         sparse_native=False):
    """Compare available channels at one fixed anchor; report gaps separately.

    missing_original_player_frames retains its historical meaning: manager records
    present without a player payload. Player coverage also includes absent managers.
    Coverage describes these traced channels, not rendering, sound, or all game state.
    sparse_native permits independent native channel extents. Missing/compared
    lists use original-relative frames; dependency records also name absolute
    native frames. Observable coverage counts source-eligible comparisons, not
    missing original payloads (reported separately). No values fill native gaps.
    """
    require(total > 0 and first_frame > 1, 'Invalid interval/preceding native frame')
    channels = dict(manager=managers, movement=movement, dialogue=dialogue, choice=choices,
                    camera=cameras, **{'camera-state': camera_states}, actor=actors)
    if not sparse_native:
        require(len(managers) == len(movement) == len(dialogue) == len(choices) == len(cameras) ==
                len(camera_states) == len(actors), 'Native trace extents')
    native_index = {}
    for name, records in channels.items():
        if not sparse_native:
            require([r[0] for r in records] == list(range(1, len(records) + 1)),
                    f'Native {name} frame sequence')
        previous = 0
        indexed = {}
        for row in records:
            require(row and isinstance(row[0], int) and not isinstance(row[0], bool) and
                    row[0] > previous,
                    f'Native {name} frames must be positive and strictly increasing without duplicates')
            previous = row[0]
            if first_frame - 1 <= row[0] < first_frame + total:
                indexed[row[0]] = row
        native_index[name] = indexed
    if not sparse_native:
        require(first_frame + total - 1 <= len(managers), 'Native interval incomplete')
    manager_by_frame, missing_manager = indexed_observations(transitions, total, 'Original manager')
    camera_by_frame, missing_camera = indexed_observations(camera, total, 'Original camera')
    require(all(0 <= f < total for f in dispatch_frames), 'Original dispatch frame outside interval')
    require(len(controller) % 2 == 0 and len(controller) == len(original_choices), 'Controller extent')
    differences = {key: [] for key in ('controller', 'choice', 'controller_manager_pad_gate',
                   'manager_running_pc_gate',
                   'pad_latch', 'direction_latch', 'player_mask', 'hero_facing_w', 'hero_position',
                   'camera_rotation', 'camera_mode_phase', 'camera_position', 'camera_unblended_target')}
    compared = {key: [] for key in differences}
    compared['controller_manager_pad_gate'] = []
    missing_dependencies = {key: {} for key in compared}

    def need(observable, f, *dependencies):
        missing = [dict(channel=name, native_frame=tick) for name, tick in dependencies
                   if tick not in native_index[name]]
        if missing:
            existing = missing_dependencies[observable].setdefault(f, [])
            existing.extend(item for item in missing if item not in existing)
            return None
        return tuple(native_index[name][tick] for name, tick in dependencies)

    def checked(observable, f):
        compared[observable].append(f)

    observed = []
    for before, after, cb, ca in zip(controller[::2], controller[1::2],
                                     original_choices[::2], original_choices[1::2]):
        f = after[0]
        require(before[:2] == cb[:2] == (f, 0x148dc8) and
                after[:2] == ca[:2] == (f, 0x11ff38), 'Controller pairing')
        require(0 <= f < total and (not observed or f > observed[-1]), 'Controller frame order')
        observed.append(f)
        current = first_frame + f
        expected = tuple(after[i] for i in (32, 33, 34, 35, 36, 16, 21, 17, 7, 43))
        records = need('controller', f, ('dialogue', current))
        if records is not None:
            checked('controller', f)
            if records[0][2:] != expected:
                differences['controller'].append(f)
        records = need('choice', f, ('choice', current))
        if records is not None:
            checked('choice', f)
            if records[0][2:6] != ca[2:6]:
                differences['choice'].append(f)
        records = need('controller_manager_pad_gate', f, ('manager', current))
        if records is not None:
            checked('controller_manager_pad_gate', f)
            if records[0][1] != after[9]:
                differences['controller_manager_pad_gate'].append(f)

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
        current = first_frame + f
        records = need('manager_running_pc_gate', f, ('manager', current - 1))
        if records is not None:
            prior = records[0]
            checked('manager_running_pc_gate', f)
            if (prior[4], prior[7], prior[6]) != (int(bool(r[35] & 256)), r[36], r[118]):
                differences['manager_running_pc_gate'].append(f)
        records = need('pad_latch', f, ('manager', current))
        if records is not None:
            checked('pad_latch', f)
            if records[0][1] != r[33] >> 16:
                differences['pad_latch'].append(f)
        records = need('direction_latch', f, ('movement', current))
        if records is not None:
            checked('direction_latch', f)
            if records[0][1] & 0xf000 != r[33] & 0xf000:
                differences['direction_latch'].append(f)
        if r[28]:
            records = need('player_mask', f, ('manager', current - 1))
            if records is not None:
                checked('player_mask', f)
                if records[0][5] != r[48]:
                    differences['player_mask'].append(f)
            records = need('hero_facing_w', f, ('actor', current - 1))
            position_records = need('hero_position', f, ('actor', current - 1))
            if records is not None:
                hero = records[0][8:16]
                checked('hero_facing_w', f)
                if hero[:4] != r[40:44] or hero[7] != r[47]:
                    differences['hero_facing_w'].append(f)
            if position_records is not None:
                checked('hero_position', f)
                e = error(position_records[0][12:15], r[44:47])
                hero_error = max(hero_error, e)
                if e > 0.00002:
                    differences['hero_position'].append(f)
        else:
            missing_player.append(f)
    for c in camera:
        f = c[0]
        current = first_frame + f
        r = manager_by_frame.get(f)
        previous_camera = camera_by_frame.get(f - 1)
        records = need('camera_rotation', f, ('camera', current))
        if records is not None:
            checked('camera_rotation', f)
            if records[0][12:16] != c[8:12]:
                differences['camera_rotation'].append(f)
        records = need('camera_mode_phase', f, ('camera-state', current))
        if records is not None:
            checked('camera_mode_phase', f)
            if (records[0][1], records[0][3]) != c[2:4]:
                differences['camera_mode_phase'].append(f)
        records = need('camera_position', f, ('camera', current))
        if records is not None:
            checked('camera_position', f)
            e = error(records[0][4:8], c[4:8])
            camera_error = max(camera_error, e)
            if e > 0.00002:
                differences['camera_position'].append(f)
        if not (unblended_target and r is not None and previous_camera is not None and c[2] == 2 and
                not r[35] & 256 and f not in dispatch_frames and
                c[3] == previous_camera[3] == 0x3f800000 and
                c[16:24] == previous_camera[16:24] and c[28] == previous_camera[28]):
            continue
        records = need('camera_unblended_target', f, ('manager', current))
        if records is None or records[0][4]:
            continue
        records = need('camera_unblended_target', f, ('camera-state', current))
        if records is None or records[0][1] != c[2] or records[0][3] != 0x3f800000:
            continue
        records = need('camera_unblended_target', f, ('camera-state', current - 1))
        if records is None or records[0][3] != 0x3f800000:
            continue
        records = need('camera_unblended_target', f, ('camera', current))
        if records is not None:
            target_frames.append(f)
            checked('camera_unblended_target', f)
            if records[0][8:12] != c[12:16]:
                differences['camera_unblended_target'].append(f)
    failed = sorted({f for values in differences.values() for f in values})
    tail = max(failed + missing_player + missing_manager + missing_camera, default=-1) + 1
    missing_by_channel = dict(manager=missing_manager, camera=missing_camera,
                              player=sorted(set(missing_manager + missing_player)))
    coverage = {name: dict(expected_frames=total, observed_frames=total-len(missing))
                for name, missing in missing_by_channel.items()}
    result = dict(controller_samples=len(observed),
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

    if sparse_native:
        missing_native = {name: [f for f in range(total) if first_frame + f not in indexed]
                          for name, indexed in native_index.items()}
        missing_observables = {name: sorted(missing) for name, missing in missing_dependencies.items()}
        blocked = {f for gaps in missing_native.values() for f in gaps}
        blocked.update(f for gaps in missing_observables.values() for f in gaps)
        tail = max([tail - 1, *blocked]) + 1
        result['matching_tail'] = dict(first_original_frame=tail, frames=total-tail,
                                      controller_samples=sum(f >= tail for f in compared['controller']))
        result.update(
            sparse_native=True,
            native_frame_mapping=dict(current='first_frame + original_frame',
                                      prior='first_frame + original_frame - 1'),
            missing_native_frames_by_channel=missing_native,
            native_observation_coverage={name: dict(expected_frames=total,
                observed_frames=total-len(missing), missing_frames=len(missing))
                for name, missing in missing_native.items()},
            compared_observable_frames=compared,
            missing_native_observable_frames=missing_observables,
            missing_native_dependencies={name: [dict(original_frame=f, dependencies=deps)
                for f, deps in sorted(missing.items())] for name, missing in missing_dependencies.items()},
            observable_coverage={name: dict(expected_observations=len(compared[name])+len(missing),
                compared_observations=len(compared[name]), missing_native_observations=len(missing))
                for name, missing in missing_observables.items()},
            complete_native_observation_coverage=not blocked,
            complete_observation_coverage=not blocked and not any(missing_by_channel.values()))
    return result

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--first-frame', type=int, required=True)
    p.add_argument('--no-dialogue', action='store_true', help='Require empty original controller/choice traces for an interval without dialogue')
    p.add_argument('--unblended-target', action='store_true', help='Compare idle mode2 targets after stable completed blending in both traces')
    p.add_argument('--sparse-native', action='store_true', help='Index real native frame labels across gaps and report missing dependencies')
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
        unblended_target=a.unblended_target, sparse_native=a.sparse_native)
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
    if a.sparse_native:
        print(json.dumps({k: report[k] for k in ('complete_native_observation_coverage',
              'complete_observation_coverage', 'native_observation_coverage', 'observable_coverage')}))
    print('Difference counts:', {k: len(v) for k, v in compared['difference_frames'].items()})

if __name__ == '__main__':
    main()
