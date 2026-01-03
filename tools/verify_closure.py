#!/usr/bin/env python3
"""Compare native two-layer dialogue closure and exercise its field continuation."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from verify_animation import sha

def run(*args):
    subprocess.run([str(a) for a in args], check=True)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=Path('build/native'))
    p.add_argument('--output', type=Path, default=Path('local/closure-validation'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    reference = Path('local/reference-close-1140-v1.bin')
    records = list(struct.iter_unpack('<32I', reference.read_bytes()))
    if not records or len(records) % 2:
        raise ValueError('Incomplete lifecycle capture')
    for before, after in zip(records[::2], records[1::2]):
        if before[1] != 0x11fec8 or after[1] != 0x11ff5c or before[0] != after[0] or before[2] != after[2]:
            raise ValueError('Unpaired lifecycle observations')
    after = records[1::2]
    first = next(i for i, r in enumerate(after) if r[5] == 6)
    last = next(i for i in range(first, len(after)) if after[i][4] & 0x80000000)
    samples = after[first:last+1]
    if [r[0] for r in samples] != list(range(samples[0][0], samples[-1][0]+1)):
        raise ValueError('Discontinuous closing interval')
    if any(r[6] != 2 or r[7] != 1 for r in samples):
        raise ValueError('Unexpected scheduler layer/stage-slot layout')
    expected = b''.join(struct.pack('<10I', r[5], r[4], *r[13:17], *r[25:29]) for r in samples)
    fixture = args.output/'initial.bin'
    fixture.write_bytes(expected[:40])
    native = args.build/'okage-assets'
    run(native, 'dialogue-close', fixture, len(samples)-1, args.output/'native.bin')
    if (args.output/'native.bin').read_bytes() != expected:
        raise ValueError('Native shared stage/message flags/opacity mismatch')
    original_wait = Path('local/reference-close-wait-1140-v1.bin')
    waits = list(struct.iter_unpack('<16I', original_wait.read_bytes()))
    closing_waits = []
    for r in waits:
        if r[0] != 0:
            break
        closing_waits.append(r)
    if len(closing_waits) != 125 or closing_waits[-3][7] & 0x80000000 == 0:
        raise ValueError('Original close-wait scope differs')
    if [(r[12], r[15]) for r in closing_waits[-3:]] != [(2, 0xffffffff), (1, 0xffffffff), (0, 0)]:
        raise ValueError('Original close-wait countdown differs')
    app = args.build/'okage-native.app/Contents/MacOS/okage-native'
    capture = args.output/'frame-520.png'
    run(app, 'local/font-input/CMNDATA/FONT', 'local/text-input/FLDDATA/MES/FIELD.PME',
        '--scene', 'local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF/a.xmp',
        '--event', 'local/xpf-decoded/FLDDATA/EVENT/EVDATA.XPF/e_home1c.xmt',
        '--actor', 'local/xpf-decoded/FLDDATA/OBJ/MAM.XPF/a.xmd', '--frames', 520,
        '--press', '100,140,180,220,260,300,340,380,420,460,500', '--capture', capture)
    state = json.loads(Path(str(capture)+'.json').read_text())
    camera = list(struct.iter_unpack('<16I', Path(str(capture)+'.camera.bin').read_bytes()))
    if state['controller_stage'] != 9 or state['message_flags'] != 0x80000000 or state['close_frame'] != 419 or state['close_wait_release_frame'] != 422:
        raise ValueError('Unexpected native message completion')
    if len(camera) != 520 or camera[-1][1:3] != (0xc4c, 0x3b):
        raise ValueError('Unexpected native continuation')
    if camera[420][1] != 0xc48 or camera[421][1] != 0xc4c:
        raise ValueError('Native wait did not release on the observed countdown boundary')
    report = {
        'scope': 'Two-layer closing scheduler from initial stage 6 plus native opening integration; not full window lifecycle or cold startup',
        'native_tool_sha256': sha(native), 'native_app_sha256': sha(app),
        'elf_sha256': sha(Path('local/extracted/SCUS_971.29')),
        'reference_sha256': sha(reference), 'reference_wait_sha256': sha(original_wait),
        'reference_first_frame': samples[0][0], 'reference_last_frame': samples[-1][0],
        'initial_state_samples': 1, 'simulated_frames': len(samples)-1,
        'exact_words_per_frame': 10, 'native_closure_sha256': sha(args.output/'native.bin'),
        'comparison': 'Bit-exact shared stage, message flags and both layers opacity value/bounds/delta',
        'reference_wait_tail': [[r[12], r[15]] for r in closing_waits[-3:]],
        'native_state': state, 'native_event_pc': '0xc4c', 'pending_opcode': '0x3b', 'pending_actor': -9,
        'limits': ['The comparison seeds one stage-6 state; full text/input timing is not compared by this experiment',
                   'Native integrated start/input schedule differs from retail; frame counts are not retail boot frames',
                   'Negative completion status releases the original wait; general resource teardown/recreation remains unported',
                   'Actor -9 visibility and subsequent message are explicitly pending',
                   'No faithful windows, cold start, audio, battles, transitions or native saves'],
    }
    (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
