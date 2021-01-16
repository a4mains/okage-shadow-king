#!/usr/bin/env python3
"""Compare native asset-initialized XSEQ state with completed original observations."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact

def sha(data):
    return hashlib.sha256(data).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, default=Path('local/reference-music-600-v4.bin'))
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--output', type=Path, default=Path('local/sequence-validation'))
    args = parser.parse_args()
    sequence = Path('local/audio-input/SNDDATA/BGM/EVS_MSH.XSQ')
    source = sequence.read_bytes()
    if sha(source) != '908a259c61b333bad7c570b7c1c33f9e954d5c93811be52228643b8323131cf2':
        raise RuntimeError('Opening sequence hash mismatch')
    reference = args.reference.read_bytes()
    if not reference or len(reference) % 736:
        raise RuntimeError('Reference record extent')
    log = args.reference.with_suffix('.log').read_text()
    if 'Stopped: vblanks=' not in log or 'Unexpected reference breakpoint' in log:
        raise RuntimeError('Reference run did not complete')
    if not args.reference.with_suffix('.state.zip').is_file():
        raise RuntimeError('Reference lacks final state (run may have failed after stopping)')
    rows = list(struct.iter_unpack('<184I', reference))
    phases = {0x4c9c: 0, 0x4cec: 1, 0x4cf8: 1, 0x4a30: 2}
    tick = 0
    active = False
    for row in rows:
        phase = phases[row[1]]
        if phase == 0:
            if active:
                raise RuntimeError('Nested tick')
            tick += 1
            active = True
        elif not active:
            raise RuntimeError('Observation outside tick')
        if row[2] != tick:
            raise RuntimeError('Reference tick discontinuity')
        if phase == 2:
            active = False
    if active or rows[0][6] != 53 or rows[0][20] != 0 or not 1 <= tick <= 12000:
        raise RuntimeError('Reference is not a complete asset-initialized interval')
    args.output.mkdir(parents=True, exist_ok=True)
    native_path = args.output / 'native.bin'
    subprocess.run([str(args.native), 'music-sequence', str(sequence), str(tick), str(native_path)], check=True)
    native = native_path.read_bytes()
    if len(native) != len(rows)*680:
        raise RuntimeError('Native/reference event count differs')
    names = ['tick', 'phase', 'cursor', 'remaining', 'tempo', 'delta_scale', 'clock_step',
             'running_status', 'dispatch_status', 'ended']
    names += ['channel%d.word%d' % (c, w) for c in range(16) for w in range(10)]
    for index, (row, actual) in enumerate(zip(rows, struct.iter_unpack('<170I', native))):
        expected = (row[2], phases[row[1]], row[6], row[20], row[12], row[13], row[14],
                    row[18] >> 24, row[8], row[18] >> 16 & 255) + row[24:]
        for name, a, b in zip(names, actual, expected):
            if a != b:
                raise RuntimeError('Record %d tick %d %s: native %08x original %08x' % (index, row[2], name, a, b))
    events = [r for r in rows if phases[r[1]] == 1]
    report = {
        'scope': "Asset-initialized music sequencing only; no voice, mixer, device, or hardware fidelity claim",
        'sequence_sha256': sha(source),
        'driver_sha256': '7fe8b5a33c2f8700d1f964cf5ce2f66942f8f97d952e86e6dc0ad5d4f74becb2',
        'reference': str(args.reference), 'reference_sha256': sha(reference),
        'reference_log_sha256': sha(log.encode()), 'native_sha256': sha(native),
        'ticks': tick, 'records': len(rows), 'events': len(events),
        'event_status_counts': dict(sorted(Counter(hex(r[8]) for r in events).items())),
        'backward_event_cursor_jumps': sum(b[6] < a[6] for a, b in zip(events, events[1:])),
        'compared_words_per_record': 170, 'all_compared_words_exact': True,
        'excluded': ['RAM pointers', 'active voice counts', 'slot/track flags and external API fields outside channel records'],
    }
    write_exact(args.output / 'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
