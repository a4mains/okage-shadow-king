#!/usr/bin/env python3
"""Compare native ordinary-bank writes with original event opcode 0x47."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--reference', type=Path, help='Completed retail.bin to reuse')
    parser.add_argument('--output', type=Path, default=Path('local/event-write-validation'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cases = [(0x10007, 1), (0x10007, 0), (0x10007, -1), (0, 0), (7, 1),
             (0x107ff, -1), (0x20000, 255), (0x201ff, -128), (0x30000, 256),
             (0x300ff, -1), (0x40000, -2048), (0x40007, 2047)]
    fixture = args.output / 'input.bin'
    write_exact(fixture, b''.join(struct.pack('<Ii', *case) for case in cases))
    reference = args.reference or args.output / 'retail.bin'
    elf, ram = Path('local/extracted/SCUS_971.29'), Path('local/text-validation-v4/opening600-ee.bin')
    if not args.reference:
        subprocess.run(['build/play-research/okage-oracle/okage-pose-oracle', 'event-write',
                        str(elf), str(ram), str(fixture), str(reference)], check=True)
    output = args.output / 'native.bin'
    subprocess.run([str(args.native), 'event-write', str(fixture), str(output)], check=True)
    expected = reference.read_bytes()
    if len(expected) != len(cases) * 4100 or expected != output.read_bytes():
        raise RuntimeError('Original/native variable mutation or continuation mismatch')
    for index in range(len(cases)):
        if struct.unpack_from('<I', expected, index * 4100)[0] != 4:
            raise RuntimeError('Original handler did not consume one selector word')
    report = {
        'scope': 'Ordinary-bank variable storage and original handler selector consumption; field dispatch integration separately exercised by field-tests',
        'original_handler': '0x130270', 'storage_function': '0x125918',
        'preconditions': 'Captured EE state supplies the original logger. Synthetic variable/field buffers. Unpatched instructions; pinned Play BIOS allows only bounded queries/uncontended semaphore services and rejects rescheduling.',
        'excluded': ['bank 4/index 6 controller side effect', 'special banks with bit 8', 'cold initialization', 'persistence'],
        'cases': [{'selector': hex(selector), 'value': value} for selector, value in cases],
        'variable_bytes_compared_per_case': 4096, 'field_state_bytes_asserted_unchanged_per_case': 2048,
        'original_selector_bytes_consumed': 4, 'exact': True,
        'hashes': {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                   for path in [elf, ram, fixture, reference, output]}}
    write_exact(args.output / 'report.json', (json.dumps(report, indent=2) + '\n').encode())
    print('Twelve original variable writes and selector consumption match')

if __name__ == '__main__':
    main()
