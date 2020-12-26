#!/usr/bin/env python3
"""Compare native dialogue input, frame progression, wait and opacity behavior."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_text_frames import frame_outputs

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--output', type=Path, default=Path('local/controller-validation'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    controller = Path('local/reference-controller-760-v2.bin')
    text = Path('local/reference-controller-text-760-v2.bin')
    rows = list(struct.iter_unpack('<64I', controller.read_bytes()))
    trace = list(struct.iter_unpack('<40I', text.read_bytes()))
    if len(rows) != 320:
        raise ValueError('Expected 160 completed controller entry/return pairs')
    previous_held = 0
    for i in range(160):
        before, after = rows[2*i:2*i+2]
        if before[:2] != (i, 0x148dc8) or after[:2] != (i, 0x11ff38):
            raise ValueError('Controller frame pairing mismatch')
        if before[9] != before[8] & ~previous_held:
            raise ValueError('Unexpected pad edge state')
        previous_held = before[8]
        if before[3:5] != (1, 1) or before[6:8] != (0, 5) or before[12:16] != (1, 0, 0, 0) or before[18] or before[19:21] != (0xffffffff, 0xffffffff) or before[62] or before[24:26] != (0x40, 4):
            raise ValueError('Controller capture outside implemented input scope')
    subprocess.run([str(args.native), 'dialogue-replay', 'local/text-input/FLDDATA/MES/FIELD.PME', '10100',
                    'local/font-input/CMNDATA/FONT', str(text), str(controller), str(args.output)], check=True)
    frames = list(struct.iter_unpack('<23I', (args.output/'frames.bin').read_bytes()))
    xy = {r[0]: r[29:31] for r in trace if r[1] in (0x198390, 0x1984f4)}
    expected = []
    for i, frame in enumerate(frame_outputs(trace)):
        r = rows[2*i+1]
        expected.append(frame+xy[i]+(r[32], r[33], r[34], r[35], r[36], r[16], r[21], r[17], r[7], r[43]))
    if frames != expected:
        raise ValueError('Native controller/frame mismatch')
    original_wait = list(struct.iter_unpack('<16I', Path('local/reference-dialogue-760-v2.bin').read_bytes()))
    native_wait = list(struct.iter_unpack('<4I', (args.output/'wait.bin').read_bytes()))
    if native_wait != [r[12:] for r in original_wait]:
        raise ValueError('Native controller-driven event wait mismatch')
    fixtures = [(value, 128., 0., step) for value in (-1., -0., 0., .1, 1., 4., 120., 128., 129.)
                for step in (-128., -8., -4., -.2, -0., 0., .2, 8., 128.)]
    fixtures += [(value, 1., -1., step) for value in (-1., -.5, 0., .5, 1.) for step in (-.5, 0., .5)]
    source = args.output/'opacity-input.bin'
    write_exact(source, b''.join(struct.pack('<4f', *r) for r in fixtures))
    for label, command in (('native', [str(args.native)]),
                           ('retail', ['build/play-research/okage-oracle/okage-pose-oracle'])):
        extra = ['local/extracted/SCUS_971.29'] if label == 'retail' else []
        subprocess.run(command+['dialogue-opacity']+extra+[str(source), str(args.output/('opacity.'+label))], check=True)
    if (args.output/'opacity.native').read_bytes() != (args.output/'opacity.retail').read_bytes():
        raise ValueError('Original opacity mismatch')
    report = {'scope': "Normal dialogue input/progression, frame cursor/layout and event wait; excludes full controller choices, audio/effects, field initialization and final window drawing",
              'native_sha256': digest(args.native), 'controller_capture_sha256': digest(controller), 'text_capture_sha256': digest(text),
              'frames': len(frames), 'exact_words_per_frame': 23, 'frames_sha256': digest(args.output/'frames.bin'),
              'input': 'One initial captured state plus held/pressed pad words; no public resume/period actions or later state corrections',
              'pressed_frames': [r[0] for r in rows[::2] if r[9]],
              'native_advance_counts_after_presses': [frames[r[0]][18] for r in rows[::2] if r[9]],
              'event_wait_calls': len(native_wait), 'event_wait_exact': True, 'wait_sha256': digest(args.output/'wait.bin'),
              'opacity_cases': len(fixtures), 'opacity_exact': True, 'opacity_sha256': digest(args.output/'opacity.native'),
              'limits': 'Pad words are observed at the game controller interface; raw device polling latency is not reproduced. Window layer readiness is seeded and remains constant in this interval. Opacity is independently validated, but constant in the full-game capture.'}
    (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
