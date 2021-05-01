#!/usr/bin/env python3
"""Audit CMP dialogue branches against 228 completed original handler calls.

Covers signed <, >, =, != with bit banks0/1 and integer bank4, including
signed limits. Native cases run the controller and PME branch selection.
This does not establish whole-message timing, window rendering or all metadata.
"""
import argparse
import itertools
import json
from pathlib import Path
import struct
import subprocess
from prepare_assets import ELF_HASH
from verify_event import digest, write_exact
from verify_message_opening import require

CASES_SHA = 'c3c3be39297b1978aa2fe1f0e70f973620dd23a1c7f54bf39951312154d4fdfe'
ORIGINAL_SHA = '4b7a3c0439ab9656704d61c3ac925949beab3d8e3549bf50c28a7146ec46fd80'

def cases():
    result = []
    for bank in [0, 1, 4]:
        values = [0, 1] if bank != 4 else [-2147483648, -2, -1, 0, 1, 2, 2147483647]
        for op, value, rhs in itertools.product(range(4), values, values):
            result.append(struct.pack('<4I', op, bank, value & 0xffffffff, rhs & 0xffffffff))
    return b''.join(result)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--prepare', action='store_true', help='Write cases only; no original execution')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    fixture = args.output / 'cases.bin'
    write_exact(fixture, cases())
    require(digest(fixture) == CASES_SHA, 'Changed comparison fixture')
    if args.prepare:
        return
    elf = Path('local/extracted/SCUS_971.29')
    original = Path('local/comparison-options-v1/original.bin')
    manifest = original.with_name('original.run.json')
    run = json.loads(manifest.read_text())
    require(digest(elf) == ELF_HASH, 'Changed retail ELF')
    require(run['exit_code'] == 0 and run['sha256'][str(elf)] == ELF_HASH,
            'Incomplete/wrong original comparison run')
    require(run['sha256']['local/comparison-options-v1/cases.bin'] == CASES_SHA,
            'Changed original input')
    require(digest(original) == ORIGINAL_SHA and original.stat().st_size == 228 * 4,
            'Changed original comparison output')
    native = args.output / 'native.bin'
    command = [str(args.native), 'comparison-options', str(fixture), str(native)]
    subprocess.run(command, check=True)
    require(native.read_bytes() == original.read_bytes(), 'Native comparison branches differ')
    report = dict(scope=__doc__, cases=228, exact_handler_result_words=228,
                  native_controller_checks=['branch replacement', 'one resume', 'no advance increment', 'delay cleared'],
                  original_run=run, native_command=command,
                  sha256={str(p): digest(p) for p in [fixture, original, manifest, native, args.native]})
    write_exact(args.output / 'report.json', (json.dumps(report, indent=2) + '\n').encode())
    print('228 original/native comparison branches match')

if __name__ == '__main__':
    main()
