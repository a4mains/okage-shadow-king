#!/usr/bin/env python3
"""Compare HOME1C XCL relocation and candidate queries with original execution."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
from verify_event import write_exact

def queries():
    rng = random.Random(731)
    fixed = [(-100, -100, -100, 100, 100, 100),
             (0, -6, -10, 4, 1, -5),
             (-24, -8, -24, -24, -8, -24),
             (-24.01, -8.01, -24.01, -23.99, -7.99, -23.99),
             (24, 0, 24, -24, -8, -24)]
    rows = []
    for mode in range(3):
        rows.extend((*box, mode) for box in fixed)
        for _ in range(60):
            first = [rng.uniform(-35, 35), rng.uniform(-12, 5), rng.uniform(-35, 35)]
            second = [v + rng.uniform(-5, 5) for v in first]
            rows.append((*first, *second, mode))
    return b''.join(struct.pack('<6fI', *row) for row in rows)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--oracle', type=Path, default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    parser.add_argument('--reference', type=Path, help='Reuse completed retail.bin for these exact queries')
    parser.add_argument('--output', type=Path, default=Path('local/collision-validation'))
    parser.add_argument('--corpus', action='store_true', help='Also structurally import every decoded owned XCL')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    elf = Path('local/extracted/SCUS_971.29')
    asset = Path('local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF/a.xcl')
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    if digest(elf) != '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5':
        raise RuntimeError('Unexpected retail ELF')
    fixture = args.output / 'queries.bin'
    query_data = queries()
    write_exact(fixture, query_data)
    reference = args.reference or args.output / 'retail.bin'
    if not args.reference:
        subprocess.run([str(args.oracle), 'xcl-probe', str(elf), str(asset),
                        str(fixture), str(reference)], check=True)
    native = args.output / 'native.bin'
    subprocess.run([str(args.native), 'xcl-probe', str(asset), str(fixture), str(native)], check=True)
    expected = reference.read_bytes()
    if expected != native.read_bytes():
        raise RuntimeError('Original/native relocation or candidate order mismatch')
    payload_size, = struct.unpack_from('<I', expected)
    if payload_size != struct.unpack_from('<I', asset.read_bytes(), asset.stat().st_size - 4)[0]:
        raise RuntimeError('Unexpected payload size')
    at, counts = payload_size + 4, []
    for _ in range(len(query_data) // 28):
        count, = struct.unpack_from('<I', expected, at)
        if count > 64 or at + 4 + count * 4 > len(expected):
            raise RuntimeError('Invalid candidate list')
        counts.append(count)
        at += 4 + count * 4
    if at != len(expected):
        raise RuntimeError('Unexpected comparison trailing bytes')
    report = {
        'scope': 'HOME1C XCL relocation and sparse-grid candidate selection, not contact resolution or player movement',
        'original_functions': ['0x189028', '0x188f90', '0x189258'],
        'preconditions': 'Unpatched retail helpers run with owned XCL at synthetic base 0x900000; no captured RAM, OS or VU helpers. Six coordinates use f12..f17 and query mode uses a0.',
        'payload_bytes_exact': payload_size, 'query_cases_exact': len(counts),
        'maximum_candidates': max(counts), 'candidate_order_exact': True,
        'query_modes': [0, 1, 2], 'random_seed': 731,
        'excluded': ['contact resolution', 'movement and interaction gating', 'camera control', 'scene integration'],
        'hashes': {str(p): digest(p) for p in [elf, asset, fixture, reference, native, args.native]},
    }
    if not args.reference:
        report['oracle_sha256'] = digest(args.oracle)
    if args.corpus:
        empty = args.output / 'empty-queries.bin'
        write_exact(empty, b'')
        rows = []
        for index, path in enumerate(sorted(Path('local/xpf-decoded').rglob('*.xcl'))):
            output = args.output / ('corpus-%03d.bin' % index)
            result = subprocess.run([str(args.native), 'xcl-probe', str(path), str(empty), str(output)],
                                    capture_output=True, text=True, check=True)
            rows.append({'path': str(path), 'sha256': digest(path), 'summary': result.stdout.strip()})
        if not rows:
            raise RuntimeError('Empty XCL corpus')
        report['structural_imports'] = rows
    write_exact(args.output / 'report.json', (json.dumps(report, indent=2) + '\n').encode())
    print('Original relocation and %d candidate queries match exactly' % len(counts))

if __name__ == '__main__':
    main()
