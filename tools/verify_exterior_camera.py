#!/usr/bin/env python3
"""Replay completed original mode0 camera cases in the native camera manager."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact

def fixtures():
    f = lambda value: struct.unpack('<I', struct.pack('<f', value))[0]
    row = [0]*46
    row[4] = 100
    for i, value in ((5,.9979990124702454),(6,-9.8),(7,-48.09994888305664),
                     (8,.0003),(9,.9979990124702454),(10,-48.09994888305664),
                     (11,-48.09994888305664),(14,1),(21,-37.25)):
        row[i] = f(value)
    row[16], row[18], row[45] = 1, 0xffffffff, 128
    row[22:26] = list(map(f, (283.25,-37.25,689.66,1)))
    row[28:32] = row[22:26]
    row[32:36] = list(map(f, (0,.5,0,1)))
    row[36:40] = list(map(f, (283.25,-90,750,1)))
    records, names = [], []
    def add(name, changes):
        case = row.copy()
        for index, value in changes.items():
            case[index] = value
        records.append(struct.pack('<46I', *case))
        names.append(name)
    for phase in (0,.5,1):
        for clock in (-1,59,60):
            for look in (0,128,255):
                add(f'phase{phase}_clock{clock}_look{look}', {14:f(phase),1:clock & 0xffffffff,45:look})
    add('smoothed_target', {18:0,21:f(-30),27:f(-10),23:f(-35)})
    add('near_camera', {36:f(283),37:f(-80),38:f(690),14:0})
    add('preset2', {0:2,14:f(.99),18:0,21:f(-36)})
    return names, b''.join(records)

def compare(expected, actual, count):
    if len(expected) != count*184 or len(actual) != len(expected):
        raise ValueError('Exterior camera output extent')
    for i, (a, b) in enumerate(zip(struct.iter_unpack('<46I', expected), struct.iter_unpack('<46I', actual))):
        changed = [j for j, (x, y) in enumerate(zip(a, b)) if x != y]
        if changed:
            raise ValueError(f'Exterior camera case {i} differs at words {changed}')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, default=Path('local/exterior-camera-v1'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    names, data = fixtures()
    source = args.reference/'cases.bin'
    original = args.reference/'original.bin'
    if source.read_bytes() != data:
        raise ValueError('Original exterior camera inputs differ')
    expected = original.read_bytes()
    compare(expected, expected, len(names))
    args.output.mkdir(parents=True, exist_ok=True)
    inputs = args.output/'cases.bin'
    write_exact(inputs, data)
    asset = Path('local/xpf-decoded/FLDDATA/MAP/FIELD/FIELD_1D.XPF/a.xcl')
    snapshot = Path('local/research/exterior-load180-v1-ee.bin')
    oracle = Path('build/play-research/okage-oracle/okage-pose-oracle')
    artifacts = [source, original, inputs, asset, snapshot, oracle, args.reference/'original.log']
    commands = []
    for build in ('native', 'native-release', 'model-sanitize'):
        binary = Path('build')/build/'okage-assets'
        output = args.output/(build+'.bin')
        if output.exists() or output.is_symlink():
            raise ValueError('Use a fresh exterior camera output directory')
        command = [str(binary), 'exterior-camera-step', str(asset), str(inputs), str(output)]
        subprocess.run(command, check=True)
        compare(expected, output.read_bytes(), len(names))
        commands.append(command)
        artifacts.extend([binary, output])
    report = {
        'scope': 'Original mode0 dispatcher 0x136d00, follow/target updates and unpatched collision services',
        'elf_sha256': '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5',
        'cases': names, 'record_words': 46,
        'output_word_indices': [1,4,7,8,10,14,18,19,20,21]+list(range(22,28))+list(range(36,44)),
        'commands': commands,
        'oracle_command': [str(oracle), 'exterior-camera-step', 'local/extracted/SCUS_971.29',
                           str(snapshot), str(source), str(original)],
        'limits': ['Thirty synthetic one-step cases; no complete outdoor arrival/playback claim.',
                   'Reference snapshot is used only by the original helper, never the native app.',
                   'Fast-turn debug rendering and alternate camera modes are outside this comparison.',
                   'Field startup handoff/order has native integration tests, not yet a paired outdoor scene.'],
        'sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts},
    }
    write_exact(args.output/'report.json', (json.dumps(report,indent=2)+'\n').encode())
    print('30 exterior camera cases match exactly in three native builds:', args.output)

if __name__ == '__main__':
    main()
