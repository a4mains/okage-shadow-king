#!/usr/bin/env python3
"""Compare bounded native encounter-enemy steps with completed original execution.

The original fixture uses an EE snapshot only in the research oracle. Native
cases contain explicit test inputs; the app does not load either artifact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def cases():
    def number(value):
        return struct.unpack('<I', struct.pack('<f', value))[0]
    base = [0, 1, number(-.2), 123, 1, 3, 0, 0, 1, 0, 0, 0xffffffff]
    base += [number(x) for x in (-4000, -1000, -4000, 1, 0, 0, 0, 1,
                                .5, .5, .5, .5, -3900, -1000, -4000, 1,
                                0, .125, 0, 1, 0, .25, 0, 1)]
    records, names = [], []
    def add(name, **changes):
        row = base.copy()
        for index, value in changes.items():
            row[int(index[1:])] = value
        names.append(name)
        records.append(row)
    add('cooldown')
    seed = next(s for s in range(10000)
                if ((((s * 0x41c64e6d + 0x3039) & 0x7fffffff) >> 5) & 511) < 3)
    for gate, motion in ((1, 3), (0, 1), (0, 3)):
        add(f'spawn_gate{gate}_motion{motion}', w1=0, w3=seed, w4=gate, w5=motion)
    for alpha in (.005, .02, .03, .5, 1):
        for speed in (-.2, -.9995, -1):
            add(f'fade_{alpha}_{speed}', w0=0xffffffff, w1=0, w2=number(speed), w23=number(alpha))
    for counter in (1, 300):
        add(f'pursuit_{counter}', w0=counter, w1=0, w6=5)
    for height in (0, 4, 4.1):
        for mode in (0, 5):
            for steps in (0, 1):
                add(f'contact_{height}_{mode}_{steps}', w0=300, w1=0, w6=mode, w7=steps,
                    w24=number(-4002), w25=number(-1000 + height), w26=number(-4002))
    return names, b''.join(struct.pack('<36I', *r) for r in records)

def compare(expected, actual, count):
    if len(expected) != count * 144 or len(actual) != len(expected):
        raise ValueError('Enemy comparison extent mismatch')
    for index, (ref, native) in enumerate(zip(struct.iter_unpack('<36I', expected),
                                            struct.iter_unpack('<36I', actual))):
        changed = [word for word, (a, b) in enumerate(zip(ref, native)) if a != b]
        if changed:
            raise ValueError(f'Enemy case {index}: differing words {changed}')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, default=Path('local/enemy-probe-v1'))
    parser.add_argument('--output', type=Path, default=Path('local/enemy-validation'))
    args = parser.parse_args()
    names, data = cases()
    reference_input = args.reference / 'cases.bin'
    reference_output = args.reference / 'reference.bin'
    if reference_input.read_bytes() != data:
        raise ValueError('Completed oracle input does not match generated cases')
    expected = reference_output.read_bytes()
    compare(expected, expected, len(names))
    args.output.mkdir(parents=True, exist_ok=True)
    inputs = args.output / 'cases.bin'
    write_exact(inputs, data)
    commands, builds = [], []
    artifacts = [reference_input, reference_output, inputs]
    for build in ('native', 'native-release', 'model-sanitize'):
        binary = Path('build') / build / 'okage-assets'
        output = args.output / (build + '.bin')
        if output.exists() or output.is_symlink():
            raise ValueError('Use a fresh audit output directory')
        command = [str(binary), 'enemy-step', str(inputs), str(output)]
        subprocess.run(command, check=True)
        compare(expected, output.read_bytes(), len(names))
        commands.append(command)
        builds.append(build)
        artifacts.extend([binary, output])
    report = {
        'scope': 'Original 0x137218 encounter-enemy step including unmodified called services.',
        'elf_sha256': '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5',
        'cases': names, 'record_words': 36, 'exact_output_words_per_case': 21,
        'output_word_indices': [0, 1, 2, 3, 6, 8, 9, 10, 11] + list(range(12, 24)),
        'matching_builds': builds,
        'oracle_command': ['build/play-research/okage-oracle/okage-pose-oracle', 'enemy-step',
                           'local/extracted/SCUS_971.29', 'local/research/exterior-load180-v1-ee.bin',
                           str(reference_input), str(reference_output)],
        'commands': commands,
        'limits': [
            'Synthetic one-step inputs, not a complete native outdoor scene or natural encounter.',
            'Pursuit fixtures lie outside map geometry; integrated collisions need scene validation.',
            'Battle comparison covers requested mode/column/rear-attack state, not battle playback or database resolution.',
            'Oracle snapshot and cases are research-only; native session still starts with diagnostic RNG seed1.',
            'Other shared RNG callers, visible enemy rendering and field camera/music remain integration work.',
        ],
        'sha256': {str(p): digest(p) for p in artifacts},
    }
    report['sha256']['local/research/exterior-load180-v1-ee.bin'] = digest(Path('local/research/exterior-load180-v1-ee.bin'))
    write_exact(args.output / 'report.json', (json.dumps(report, indent=2) + '\n').encode())
    print(f'{len(names)} enemy cases match21 output words plus15 context words in three native builds: {args.output / "report.json"}')

if __name__ == '__main__':
    main()
