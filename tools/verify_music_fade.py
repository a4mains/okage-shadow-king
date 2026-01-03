#!/usr/bin/env python3
"""Compare native music fade with bounded execution of original EE request/update."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from prepare_assets import ELF_HASH
from verify_event import write_exact

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--oracle', type=Path, default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--output', type=Path, default=Path('local/music-fade-validation'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cases = [(128 << 16, 80, 64, 32)]
    for current in [0, 1, 40 << 16, (80 << 16) + 1, 128 << 16]:
        for target in [0, 40, 80, 128]:
            for rate in [0, 1, 16, 64, 65, 2048, 32767]:
                cases.append((current, target, rate, 48))
    source = args.output / 'cases.bin'
    write_exact(source, b''.join(struct.pack('<4I', *r) for r in cases))
    original, native = args.output / 'reference.bin', args.output / 'native.bin'
    subprocess.run([str(args.oracle), 'music-fade', 'local/extracted/SCUS_971.29', str(source), str(original)], check=True)
    subprocess.run([str(args.native), 'music-fade', str(source), str(native)], check=True)
    expected, actual = original.read_bytes(), native.read_bytes()
    if len(actual) != sum(r[3] for r in cases) * 28 or actual != expected:
        raise RuntimeError('Original/native fade observations differ')
    report = {'elf_sha256': ELF_HASH, 'cases': len(cases), 'ticks': len(actual)//28,
              'words_per_tick': 7, 'exact': True, 'sha256': hashlib.sha256(actual).hexdigest(),
              'fields': ['current', 'target', 'step', 'stop_requested', 'gain_sent', 'left', 'right'],
              'original_functions': ['0x12d778', '0x12ce20', '0x12d470'],
              'scope': 'Original request/update/stop state with unrelated pending flags zero and active track3; research-only logging/RPC/load no-ops, main-thread predicate0 and recording driver gain/stop stubs. Does not validate RPC side effects or full-game timing.'}
    write_exact(args.output / 'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
