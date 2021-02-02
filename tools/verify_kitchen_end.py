#!/usr/bin/env python3
"""Audit completed native kitchen-event captures; free movement is not covered."""
import argparse
import hashlib
import json
from pathlib import Path
from verify_choice_scene import rows, same_family
from verify_effect_scene import accepted
from verify_event import write_exact

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('local/kitchen-end-validation.json'))
    args = parser.parse_args()
    prefix = Path('local/native-kitchen-end-6000-v1.png')
    traces = ['.audio.pcm', '.music.bin', '.sound.bin', '.camera.bin', '.actor.bin',
              '.actor-9.bin', '.actor-1.bin', '.dialogue.bin', '.choices.bin', '.effects.bin']
    family = ['', '.json', '.audio.json'] + traces
    for variant in ('release', 'sanitize'):
        same_family(prefix, Path(f'local/native-kitchen-end-6000-{variant}-v1.png'), family)
    for suffix in traces:
        old = Path('local/native-third-answer-4600-v1.png' + suffix).read_bytes()
        if not Path(str(prefix) + suffix).read_bytes().startswith(old):
            raise RuntimeError('Earlier native path changed: ' + suffix)
    state = json.loads(Path(str(prefix) + '.json').read_text())
    expected = {'frames': 6000, 'message_id': 10106, 'controller_stage': 9,
                'text_progress': 12, 'field_event_running': False, 'field_camera_mode': 0,
                'kitchen_flag_7': 1, 'progression_flag_3': 1, 'progression_flag_4': 1,
                'progression_flag_5': 1}
    if any(state[key] != value for key, value in expected.items()):
        raise RuntimeError('Native end state changed')
    if accepted(prefix) != [(2720, 10102, 1), (3640, 10105, 2), (4400, 10106, 0)]:
        raise RuntimeError('Choice path changed')
    camera = rows(Path(str(prefix) + '.camera.bin'), 16)
    if len(camera) != 6000 or camera[-1][1:3] != (0x106c, 0):
        raise RuntimeError('Unexpected final event continuation')
    report = {'scope': "Native scripted kitchen event ends; no player control, collision, cold startup or full-game playability",
              'end_state': expected, 'retained_event_pc': '0x106c',
              'pc_note': 'Original XMT semantics retain the last yielded PC when opcode 0 clears the running flag; not stuck at a wait',
              'all_three_builds_exact': True, 'cross_build_suffixes': family,
              'previous_4600_prefix_exact': traces,
              'camera_evidence': 'Mode-0 handler/update reviewed in original assembly; native dispatch tested. Whole-game camera handoff comparison remains pending.',
              'physical_input_or_listening_verified': False,
              'hashes': {str(prefix) + suffix: hashlib.sha256(Path(str(prefix) + suffix).read_bytes()).hexdigest()
                         for suffix in ['', '.json', '.camera.bin', '.actor.bin']}}
    write_exact(args.output, (json.dumps(report, indent=2) + '\n').encode())
    print('Kitchen event end, three-build equality and previous-path preservation verified')

if __name__ == '__main__':
    main()
