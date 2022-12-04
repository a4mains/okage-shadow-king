#!/usr/bin/env python3
"""Check selected-capture CLI validation; every case exits before VM construction."""
import argparse
from pathlib import Path
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('oracle', type=Path, help='Newly built Vulkan boot oracle')
    args = parser.parse_args()
    oracle = args.oracle.resolve(strict=True)
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        missing_disc = root / 'not-a-disc.cue'
        prefix = root / 'capture'

        def check(options, expected):
            command = [str(oracle), str(missing_disc), '100', *options]
            result = subprocess.run(command, capture_output=True, text=True, timeout=10, cwd=root)
            if result.returncode == 0 or expected not in result.stderr:
                raise AssertionError((command, result.returncode, result.stdout, result.stderr))
            if 'Booting ' in result.stdout or (root / 'local/play-boot-config').exists():
                raise AssertionError('CLI validation unexpectedly entered VM initialization')

        for value in ('0', '101', '2,1', '1,1', '-1', '1,', ',1', '1,,2', '1x', ' 1',
                      ','.join(map(str, range(1, 66)))):
            check(['--capture', str(prefix), '--capture-frames', value], 'Capture frames')
        check(['--capture', str(prefix), '--capture-frames', ''], 'Empty capture')
        check(['--capture-frames', '1'], '--capture-frames requires --capture')
        check(['--capture', str(prefix), '--capture-frames', '1', '--capture-frames', '2'],
              'Duplicate capture')
        for value in ('1', '1,60,100', ','.join(map(str, range(1, 65)))):
            check(['--capture', str(prefix), '--capture-frames', value], 'Disc input missing')
        check(['--audio-index', str(root / 'audio.json')], '--audio-index requires --audio')
        frame_dir = Path(str(prefix) + '.frames')
        frame_dir.mkdir()
        check(['--capture', str(prefix), '--capture-frames', '1'], 'Refusing existing reference output')
        frame_dir.rmdir()
        frame_dir.symlink_to(root / 'absent-directory')
        check(['--capture', str(prefix), '--capture-frames', '1'], 'Refusing existing reference output')
        index = root / 'audio.json'
        index.symlink_to(root / 'absent-file')
        check(['--audio', str(root / 'audio.wav'), '--audio-index', str(index)],
              'Refusing existing reference output')
        wander = root / 'meg-wander.jsonl'
        check(['--wander'], 'Unknown boot argument')
        check(['--wander', str(wander), '--wander', str(wander)], 'Duplicate wander')
        check(['--wander', str(wander)], 'Disc input missing')
        wander.touch()
        check(['--wander', str(wander)], 'Refusing existing reference output')
        rng = root / 'shared-rng.jsonl'
        check(['--rng'], 'Unknown boot argument')
        check(['--rng', str(rng), '--rng', str(rng)], 'Duplicate RNG')
        check(['--rng', str(rng)], 'Disc input missing')
        rng.touch()
        check(['--rng', str(rng)], 'Refusing existing reference output')
        callbacks = root / 'battle-callbacks.jsonl'
        check(['--battle-callbacks'], 'Unknown boot argument')
        check(['--battle-callbacks', str(callbacks), '--battle-callbacks', str(callbacks)],
              'Duplicate battle-callbacks')
        check(['--battle-callbacks', str(missing_disc)], 'Battle-callback output aliases')
        check(['--battle-callbacks', str(callbacks), '--save', str(callbacks)],
              'Battle-callback output aliases')
        check(['--battle-callbacks', str(callbacks), '--battle-ui-updates', str(root / 'updates.jsonl')],
              'cannot be combined with --battle-ui-updates')
        check(['--battle-callbacks', str(callbacks)], 'Disc input missing')
        callbacks.touch()
        check(['--battle-callbacks', str(callbacks)], 'Refusing existing reference output')
        layout = root / 'ui-layout.jsonl'
        check(['--ui-layout'], 'Unknown boot argument')
        check(['--ui-layout', str(layout), '--ui-layout', str(layout)], 'Duplicate UI-layout')
        check(['--ui-layout', str(missing_disc)], 'UI-layout output aliases')
        check(['--ui-layout', str(layout), '--save', str(layout)], 'UI-layout output aliases')
        draw = root / 'ui-draw.jsonl'
        check(['--ui-draw'], 'Unknown boot argument')
        check(['--ui-draw', str(draw)], '--ui-draw requires --ui-layout')
        check(['--ui-layout', str(layout), '--ui-draw', str(draw), '--ui-draw', str(draw)], 'Duplicate UI-draw')
        check(['--ui-layout', str(layout), '--ui-draw', str(draw), '--rng', str(root / 'rng.jsonl')],
              'can only be combined with --ui-layout')
        check(['--ui-layout', str(layout), '--ui-draw', str(layout)], 'UI-draw output aliases')
        check(['--ui-layout', str(layout), '--ui-draw', str(draw), '--save', str(draw)],
              'UI-draw output aliases')
        check(['--ui-layout', str(layout), '--ui-draw', str(draw)], 'Disc input missing')
        draw.touch()
        check(['--ui-layout', str(layout), '--ui-draw', str(draw)], 'Refusing existing reference output')
        draw.unlink()
        check(['--ui-layout', str(layout), '--battle-ui-updates', str(root / 'ui-updates.jsonl')],
              'cannot be combined with --battle-callbacks')
        check(['--ui-layout', str(layout), '--battle-callbacks', str(root / 'callbacks.jsonl')],
              'cannot be combined with --battle-callbacks')
        check(['--ui-layout', str(layout), '--lifecycle', str(root / 'lifecycle.jsonl')],
              'cannot be combined with --battle-callbacks')
        check(['--ui-layout', str(layout), '--order', str(root / 'order.jsonl')],
              'cannot be combined with --battle-callbacks')
        check(['--ui-layout', str(layout), '--text', str(root / 'text.jsonl')],
              'cannot be combined with --battle-callbacks')
        layout.touch()
        check(['--ui-layout', str(layout)], 'Refusing existing reference output')
    print('Capture argument tests passed; no VM constructed.')

if __name__ == '__main__':
    main()
