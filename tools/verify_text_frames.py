#!/usr/bin/env python3
"""Compare native text pacing with the completed opening clock/presentation trace."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact

def words(rows):
    return b''.join(struct.pack('<'+'I'*len(row), *(n & 0xffffffff for n in row)) for row in rows)

def f32(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]

def clocks(rows):
    inputs, expected = [], []
    pending = None
    for r in rows:
        if r[1] == 0x1980f8:
            if pending is not None:
                raise ValueError('Missing original clock return observation')
            pending = r
        elif r[1] == 0x198390:
            if pending is None or r[0] != pending[0]:
                raise ValueError('Unpaired original clock observation')
            p = pending
            inputs.append((p[10], p[11] & 255, p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23]))
            expected.append((r[10], r[16], r[17], r[18], r[19], r[21]))
            pending = None
    if pending is not None:
        raise ValueError('Unfinished clock observation')
    return inputs, expected

def frame_outputs(rows):
    frames = {}
    for r in rows:
        if r[1] in (0x198390, 0x1984f4):
            # 0x1984f4 precedes the dispatcher's final clearing of bit 1.
            frames[r[0]] = (r[10] & ~1, r[12], r[13], r[14], int(r[15] != 0),
                            r[16], r[17], r[18], r[19], r[20], r[21])
    if sorted(frames) != list(range(160)):
        raise ValueError('Expected completed 160-vblank opening capture')
    return [frames[i] for i in range(160)]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--capture', type=Path, default=Path('local/reference-text-760-v2.bin'))
    p.add_argument('--output', type=Path, default=Path('local/text-frame-validation-v2'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    capture = args.capture.read_bytes()
    if not capture or len(capture) % 160:
        raise ValueError('Text observation record bounds')
    rows = list(struct.iter_unpack('<40I', capture))
    source, expected = clocks(rows)
    if len(source) != 160:
        raise ValueError('Incomplete clock capture')
    clock_rows = [r for r in rows if r[1] == 0x1980f8]
    if len({(r[11] & 255, r[22], r[23], r[24], r[25], r[32]) for r in clock_rows}) != 1:
        raise ValueError('Replay scope excludes external window changes')
    synthetic = []
    for period in (0, 1, 2, -1, -4, -2147483648, 2147483647):
        for phase in (0, 1, -1, 2147483647):
            for flags in (0x8008, 0x8808, 0x8108, 0x8018, 0x8028, 8):
                synthetic.append((flags, 0x10, phase, period, 0, 0xffffffff, 0, 0, f32(1), f32(1)))
    for delay in (1, 2, -1):
        for modes in (0, 0x10):
            synthetic.append((0x8008, modes, 0, 1, delay, 17, 0, 0, f32(1), f32(1)))
    for target, position, step, base in ((24, 0, 1, 1), (24, 23.5, 1, 1), (24, 10, 0, 1),
                                        (24, 10, 1, 0), (24, 10, -1, 1), (0, 10, 1, 1),
                                        (2147483647, 2147483520, 256, 1), (1, 0.1, 0.2, 1)):
        synthetic.append((0x8008, 0x10, 0, 1, 2, 17, target, f32(position), f32(step), f32(base)))
    report = {'scope': "Text clock and 160-frame cursor/progress replay; native font layout, window controller and raw pad polling not included",
              'capture_sha256': hashlib.sha256(capture).hexdigest(),
              'native_sha256': hashlib.sha256(args.native.read_bytes()).hexdigest(), 'clock_cases': []}
    for name, cases, expected_rows in (('opening', source, expected), ('synthetic', synthetic, None)):
        input_path = args.output/(name+'.clock.input')
        native = args.output/(name+'.clock.native.bin')
        retail = args.output/(name+'.clock.retail.bin')
        write_exact(input_path, words(cases))
        subprocess.run([str(args.native), 'text-clock', str(input_path), str(native)], check=True)
        subprocess.run(['build/play-research/okage-oracle/okage-pose-oracle', 'text-clock',
                        'local/extracted/SCUS_971.29', str(input_path), str(retail)], check=True)
        actual = native.read_bytes()
        if actual != retail.read_bytes():
            raise RuntimeError(name+': native/original clock mismatch')
        if expected_rows is not None and actual != words(expected_rows):
            raise RuntimeError('Native clock differs from full-game observations')
        report['clock_cases'].append({'name': name, 'calls': len(cases), 'exact': True,
                                     'output_sha256': hashlib.sha256(actual).hexdigest()})
    target = args.output/'opening.frames.native.bin'
    wait_target = args.output/'opening.wait.native.bin'
    subprocess.run([str(args.native), 'text-frame-replay', 'local/text-input/FLDDATA/MES/FIELD.PME', '10100',
                    str(args.capture), str(target), '2', str(wait_target)], check=True)
    frame_rows = frame_outputs(rows)
    expected_frames = words(frame_rows)
    write_exact(args.output/'opening.frames.retail.bin', expected_frames)
    if target.read_bytes() != expected_frames:
        a = list(struct.iter_unpack('<11I', target.read_bytes()))
        b = frame_rows
        for i, (left, right) in enumerate(zip(a, b)):
            if left != right:
                raise RuntimeError(f'Frame {i}: native {left}, original {right}')
        raise RuntimeError('Native text frame output length differs')
    report['frame_replay'] = {'frames': 160, 'exact_words_per_frame': 11,
                             'seed': 'One initial cursor/clock state plus observed resume/period API calls; no subsequent cursor/counter corrections',
                             'output_sha256': hashlib.sha256(expected_frames).hexdigest(),
                             'progress_changes': [[i, r[1]] for i, r in enumerate(frame_rows)
                                                  if i == 0 or r[1] != frame_rows[i-1][1]]}
    wait_capture = Path('local/reference-dialogue-760-v2.bin').read_bytes()
    wait_rows = list(struct.iter_unpack('<16I', wait_capture))
    if wait_rows[0][:6] != (2, 5, 1, 0, 0, 0) or any((r[7]&0x40)==0 or (r[8]&0x40)==0 for r in rows):
        raise ValueError('Opening wait fixture initialization/readiness changed')
    if wait_target.read_bytes() != words([r[12:] for r in wait_rows]):
        raise RuntimeError('Native timed text/event wait differs from original progression')
    report['event_wait'] = {'calls': len(wait_rows), 'exact': True, 'release_call_index': len(wait_rows)-1,
                            'native_progress_used': True, 'readiness': 'Both initially ready layers remain ready in this capture',
                            'output_sha256': hashlib.sha256(wait_target.read_bytes()).hexdigest()}
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
