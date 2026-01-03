#!/usr/bin/env python3
"""Audit completed native effect captures without rerunning the game."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from verify_choice_scene import rows, same_family
from verify_event import write_exact

def effect_rows(path):
    data = path.read_bytes()
    offset, result = 0, []
    while offset < len(data):
        frame, pc, count = struct.unpack_from('<3I', data, offset)
        offset += 12
        if frame != len(result) + 1 or count > 84:
            raise ValueError('Invalid effect trace header')
        effects = []
        for _ in range(count):
            effects.append(struct.unpack_from('<30I', data, offset))
            offset += 120
        result.append((frame, pc, effects))
    return result

def accepted(prefix):
    trace = rows(Path(str(prefix) + '.choices.bin'), 12)
    return [(r[0], r[1], trace[i-1][4]) for i, r in enumerate(trace)
            if i and r[2] == 0 and trace[i-1][6] and r[9] & 0x40]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('local/effect-scene-validation.json'))
    args = parser.parse_args()
    prefix = Path('local/native-effect-5000-v1.png')
    traces = ['.audio.pcm', '.music.bin', '.sound.bin', '.camera.bin',
              '.actor.bin', '.actor-9.bin', '.actor-1.bin', '.dialogue.bin', '.choices.bin']
    family = ['', '.json'] + traces + ['.effects.bin']
    for variant in ('release', 'sanitize'):
        same_family(prefix, Path(f'local/native-effect-5000-{variant}-v1.png'), family + ['.audio.json'])
    same_family(Path('local/native-effect-4180-v1.png'), Path('local/native-effect-ui-4180-v1.png'), family)
    for suffix in traces:
        old = Path('local/native-choice-third-4080-v1.png' + suffix).read_bytes()
        if not Path(str(prefix) + suffix).read_bytes().startswith(old):
            raise RuntimeError('Pre-effect regression: ' + suffix)
    effects = effect_rows(Path(str(prefix) + '.effects.bin'))
    if len(effects) != 5000:
        raise ValueError('Unexpected effect capture duration')
    active = [frame for frame, _, objects in effects if objects]
    if active != list(range(4127, 4208)):
        raise RuntimeError('Effect allocation/removal frames changed')
    for frame, _, objects in effects:
        if objects:
            e, = objects
            if e[0] != 0 or e[1] != 0xfffffff8 or e[3] != (0x8720 if frame == 4207 else 0x8420):
                raise RuntimeError('Effect identity/flags changed')
    state = json.loads(Path(str(prefix) + '.json').read_text())
    camera = rows(Path(str(prefix) + '.camera.bin'), 16)
    if state['message_id'] != 10106 or state['text_progress'] != 5 or camera[-1][1] != 0xfdc:
        raise RuntimeError('Third-choice waiting state changed')
    if accepted(prefix) != [(2720, 10102, 1), (3640, 10105, 2)]:
        raise RuntimeError('First/second choice path changed')
    alternatives = []
    for name, answer, frame, pc, progress in [
            ('native-third-answer-4600-v1', 0, 4400, 0x1010, 8),
            ('native-third-answer-two-4600-v1', 2, 4480, 0xfe4, 7)]:
        other = Path('local/' + name + '.png')
        if accepted(other) != [(2720, 10102, 1), (3640, 10105, 2), (frame, 10106, answer)]:
            raise RuntimeError('Third-choice acceptance changed')
        end = json.loads(Path(str(other) + '.json').read_text())
        if end['frames'] != 4600 or end['text_progress'] != progress or rows(Path(str(other) + '.camera.bin'), 16)[-1][1] != pc:
            raise RuntimeError('Third-choice continuation changed')
        alternatives.append({'answer': answer, 'accepted_frame': frame, 'capture': str(other),
                             'frames': 4600, 'event_pc': hex(pc), 'progress': progress})
    audio = json.loads(Path('local/native-effect-ui-4180-v1.png.audio.json').read_text())
    report = {
        'scope': 'Native type-30 scene integration and consistency; not original whole-scene timing or pixel equivalence',
        'frames': 5000, 'message': 10106, 'event_pc': '0xfdc', 'progress': 5,
        'all_three_builds_exact': True, 'cross_build_suffixes': family + ['.audio.json'],
        'previous_4080_prefix_exact': traces,
        'effect': {'actor_alias': -8, 'slot': 0, 'first_visible_frame': 4127,
                   'last_visible_frame': 4206, 'removal_marked_frame': 4207, 'slot_freed_frame': 4208},
        'third_answers': alternatives,
        'appkit': {'frames': 4180, 'headless_image_pcm_state_and_traces_exact': True,
                   'device_frames_enqueued': audio['device_frames_enqueued'],
                   'device_underrun_frames': audio['device_underrun_frames'],
                   'physical_input_or_listening_verified': False},
        'render_limits': 'Bubble appearance/position inspected qualitatively. Exact GS filtering, color quantization and alpha equivalence unverified.',
        'hashes': {str(prefix) + suffix: hashlib.sha256(Path(str(prefix) + suffix).read_bytes()).hexdigest()
                   for suffix in ['', '.effects.bin', '.choices.bin']}}
    write_exact(args.output, (json.dumps(report, indent=2) + '\n').encode())
    print('Effect lifecycle, three-build consistency, AppKit replay and two third-choice paths verified')

if __name__ == '__main__':
    main()
