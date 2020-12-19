#!/usr/bin/env python3
"""Exact native/original dialogue wait comparison, including real opening input."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact

def synthetic():
    cases = []
    for request in (-2, -1, 0, 1, 2, 2047):
        for countdown in (0, 1, 2, 0xffffffff):
            for observed, accumulated in ((0, 0), (0, 1), (1, 0), (3, 1), (0xffffffff, 0x7fffffff)):
                observations = ((5, 1, 1, 0, 2, 3, 0, 0), (5, 0, 1, 0, 2, 3, 0, 4),
                                (4, 1, 1, 0, 2, 1, 0, 4), (5, 1, 1, -1, 0, 0, 0, 3),
                                (5, 1, 1, 0, 2, 3, -1, 0), (5, 1, 0, 0, 0, 0, -1, 0))
                for mode, active, present, status, count, mask, handle, progress in observations:
                    if not present and request not in (-1, 0) and not countdown:
                        continue
                    cases.append((request, mode, active, countdown, observed, accumulated,
                                  present, status, count, mask, handle, progress))
    return b''.join(struct.pack('<12I', *(v & 0xffffffff for v in row)) for row in cases)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--output', type=Path, default=Path('local/dialogue-validation'))
    parser.add_argument('--capture', type=Path, default=Path('local/reference-dialogue-760-v2.bin'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    capture = args.capture.read_bytes()
    if not capture or len(capture) % 64:
        raise RuntimeError('Truncated or empty original dialogue trace')
    records = list(struct.iter_unpack('<16I', capture))
    # This opening case is one continuous wait, without external counter resets.
    for previous, current in zip(records, records[1:]):
        if previous[12:15] != current[3:6]:
            raise RuntimeError('Captured wait is not a continuous counter sequence')
    expected = b''.join(struct.pack('<4I', *row[12:]) for row in records)
    inputs = b''.join(struct.pack('<12I', *row[:12]) for row in records)
    report = {'scope': "Dialogue wait callback and readiness getter, not native dialogue presentation",
              'capture_sha256': hashlib.sha256(capture).hexdigest(), 'cases': []}
    for name, data, result in (('opening', inputs, expected), ('synthetic', synthetic(), None)):
        source = args.output/(name+'.input.bin')
        native = args.output/(name+'.native.bin')
        retail = args.output/(name+'.retail.bin')
        write_exact(source, data)
        subprocess.run([str(args.native), 'dialogue-wait', str(source), str(native)], check=True)
        subprocess.run(['build/play-research/okage-oracle/okage-pose-oracle', 'dialogue-wait',
                        'local/extracted/SCUS_971.29', str(source), str(retail)], check=True)
        actual = native.read_bytes()
        if actual != retail.read_bytes() or len(actual) != len(data)//3:
            raise RuntimeError(name + ': original/native dialogue result mismatch')
        if result is not None and actual != result:
            raise RuntimeError('Full-game/native dialogue return or state mismatch')
        report['cases'].append({'name': name, 'calls': len(data)//48,
                                'input_sha256': hashlib.sha256(data).hexdigest(),
                                'output_sha256': hashlib.sha256(actual).hexdigest(),
                                'full_game_exact': result is not None})
    report['release_call_indices'] = [i for i, row in enumerate(records) if row[-1] == 0]
    report['native_sha256'] = hashlib.sha256(args.native.read_bytes()).hexdigest()
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
