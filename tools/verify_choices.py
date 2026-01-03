#!/usr/bin/env python3
"""Compare native PME branches and digital repeat with isolated retail EE calls.

The reference uses the existing opening600 EE fixture; the native tools consume
only the PME and synthetic pad input. This does not validate complete UI timing.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact

def words(path):
    data = path.read_bytes()
    if len(data) % 4:
        raise ValueError('Truncated word stream')
    return list(struct.unpack('<%dI' % (len(data)//4), data))

def reference_paths(data):
    result, states, at = [], [], 0
    for _ in range(2):
        if len(data)-at < 12:
            raise ValueError('Truncated choice state')
        states.append(data[at:at+11])
        count = data[at+11]
        end = at+12+2*count
        if count > 256 or end > len(data):
            raise ValueError('Invalid choice fragment count')
        result.extend(data[at+11:end])
        at = end
    if at != len(data):
        raise ValueError('Trailing choice data')
    return result, states

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--output', type=Path, default=Path('local/choice-validation'))
    p.add_argument('--reference', type=Path, help='Reuse completed reference directory')
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    reference = args.reference or args.output
    oracle = 'build/play-research/okage-oracle/okage-pose-oracle'
    elf = 'local/extracted/SCUS_971.29'
    ram = 'local/text-validation-v4/opening600-ee.bin'
    pme = 'local/text-input/FLDDATA/MES/FIELD.PME'
    report = {'scope': 'Original branch stream/node rewriting and digital pad repeat; not a full-scene comparison',
              'native_sha256': hashlib.sha256(args.native.read_bytes()).hexdigest(),
              'elf_sha256': hashlib.sha256(Path(elf).read_bytes()).hexdigest(),
              'reference_fixture_sha256': hashlib.sha256(Path(ram).read_bytes()).hexdigest(),
              'reference_directory': str(reference),
              'pme_sha256': hashlib.sha256(Path(pme).read_bytes()).hexdigest(),
              'branches': []}
    for ident, fragment, selections in ((10102, 7, 3), (10105, 5, 3), (10105, 8, 3), (10106, 11, 3), (10106, 14, 2)):
        for selection in range(selections):
            name = f'{ident}-{fragment}-{selection}'
            native = args.output/(name+'.native.bin')
            retail = reference/(name+'.retail.bin')
            subprocess.run([str(args.native), 'pme-branch', pme, str(ident), str(fragment), str(selection), str(native)], check=True)
            if not args.reference:
                subprocess.run([oracle, 'pme-choice', elf, ram, str(ident), str(fragment), str(selection), str(retail)], check=True, stdout=subprocess.DEVNULL)
            expected, states = reference_paths(words(retail))
            if words(native) != expected:
                raise RuntimeError(name+': branch stream/node mismatch')
            if states[1][2:5] != [selection, 0, 0]:
                raise RuntimeError(name+': unexpected original result/cursor/reset')
            report['branches'].append({'message': ident, 'fragment': fragment, 'selection': selection,
                                       'exact': True, 'sha256': hashlib.sha256(native.read_bytes()).hexdigest(),
                                       'reference_sha256': hashlib.sha256(retail.read_bytes()).hexdigest()})
    helds = [0]*3+[0x1000]*70+[0]*2+[0x4000]*65+[0x5000]*35+[0x4040]*4+[0x40]*5+[0]*4
    previous = 0
    rows = []
    for held in helds:
        rows.append(struct.pack('<HH', held, held & ~previous))
        previous = held
    input_path = args.output/'repeat.input'
    write_exact(input_path, b''.join(rows))
    native = args.output/'repeat.native.bin'
    retail = reference/'repeat.retail.bin'
    subprocess.run([str(args.native), 'pad-repeat', str(input_path), str(native)], check=True)
    if not args.reference:
        subprocess.run([oracle, 'pad-repeat', elf, str(input_path), str(retail)], check=True, stdout=subprocess.DEVNULL)
    if native.read_bytes() != retail.read_bytes():
        raise RuntimeError('Digital repeat mismatch')
    report['digital_repeat'] = {'ticks': len(helds), 'exact': True, 'sha256': hashlib.sha256(native.read_bytes()).hexdigest()}
    options = reference/'options.bin'
    if not args.reference:
        subprocess.run([oracle, 'choice-options', elf, ram, str(options)], check=True, stdout=subprocess.DEVNULL)
    values = words(options)
    if values != [1, 1, 1, 2, 65538, 131074, 2, 65538, 65538, 2, 65538, 65538, 65538, 2, 2]:
        raise RuntimeError('Original option metadata result changed')
    report['option_observations'] = {'commands': ['CC', 'BC', 'BC=0', 'BC=1', 'BC!=0'], 'last_results': [0, 1, 2], 'return_words': values,
                                   'limit': 'Native currently implements CC, BC and BC=0 only. Original BC=1 also compares against zero; do not generalize equality parsing.'}
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(f'{len(report["branches"])} branch cases and {len(helds)} pad ticks match original EE execution')

if __name__ == '__main__':
    main()
