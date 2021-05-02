#!/usr/bin/env python3
"""Compare native map-effect state against completed isolated original probes.

Does not start a reference VM or validate geometry, rendering, allocation order,
removal lifetime, whole-scene scheduling or authentic startup RNG initialization.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from verify_event import write_exact

ELF_SHA = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
FIXTURES = {
    25: (8, 450, '041e1f6ef41b0be5846299b1a635dc80e7f956f152588d0d93115182016e5f29'),
    34: (14, 0, 'd166e6ba19585cf02826346cd500ef2171ff2a961195baf82e290b440b576c41'),
}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def number(word):
    return struct.unpack('<f', struct.pack('<I', word))[0]

def compare(expected, actual, template, elements, frames):
    stride = 29 + 44 * elements
    size = stride * 4 * (frames + 2)
    if len(expected) != size or len(actual) != size:
        raise ValueError('Incomplete map-effect interval')
    maxima = [0.0, 0.0, 0.0]
    differences = 0
    for step, (ref, got) in enumerate(zip(struct.iter_unpack(f'<{stride}I', expected),
                                          struct.iter_unpack(f'<{stride}I', actual))):
        if ref[:3] != (step, 0, template) or ref[4] != elements:
            raise ValueError('Wrong original map-effect record identity')
        for index, (a, b) in enumerate(zip(ref, got)):
            if a == b:
                continue
            element, channel = divmod(index - 29, 44)
            particle = template == 25 and 3 <= element < 8 and 24 <= channel < 27
            smoke = template == 19 and 0 <= element < 9 and channel in (32, 33)
            if step < 2 or not (particle or smoke):
                raise ValueError(f'Exact mismatch at record{step} word{index}: {a:x} != {b:x}')
            delta = abs(number(a) - number(b))
            if not delta <= 2e-6:
                raise ValueError(f'Particle position mismatch at record{step} word{index}')
            axis = channel - (32 if smoke else 24)
            maxima[axis] = max(maxima[axis], delta)
            differences += 1
    name = 'smoke_scale' if template == 19 else 'particle_position'
    return {'records': frames + 2, 'words_per_record': stride,
            'all_other_words_exact': True,
            name+'_max_abs_error': maxima[:2] if template == 19 else maxima,
            name+'_differing_words': differences}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--reference', type=Path, default=Path('local/map-effect-probe-v1'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    efd = Path('local/effect-input/CMNDATA/2D/TES.EFD')
    textures = Path('local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF')
    if digest(Path('local/extracted/SCUS_971.29')) != ELF_SHA:
        raise ValueError('Different retail ELF')
    report = {'scope': __doc__, 'elf_sha256': ELF_SHA, 'efd_sha256': digest(efd),
              'native_binary_sha256': digest(args.native),
              'texture_sha256': {p.name: digest(p) for p in sorted(textures.glob('*.xim'))},
              'position_tolerance': 2e-6, 'cases': []}
    for template, (elements, frames, reference_hash) in FIXTURES.items():
        source = args.reference / f'{template}.input.bin'
        reference = args.reference / f'{template}.original.bin'
        manifest = json.loads((args.reference / f'{template}.run.json').read_text())
        if manifest['exit_code'] != 0 or digest(reference) != reference_hash:
            raise ValueError('Unrecognized or incomplete original output')
        output = args.output / f'{template}.native.bin'
        command = [str(args.native), 'map-effect-probe', str(efd), str(textures),
                   str(source), str(frames), str(output)]
        result = subprocess.run(command, capture_output=True)
        write_exact(args.output / f'{template}.log', result.stdout + result.stderr)
        write_exact(args.output / f'{template}.run.json',
                    (json.dumps({'command': command, 'binary_sha256': digest(args.native),
                                 'exit_code': result.returncode}, indent=2) + '\n').encode())
        result.check_returncode()
        case = compare(reference.read_bytes(), output.read_bytes(), template, elements, frames)
        case.update(template=template, input_sha256=digest(source), reference_sha256=reference_hash,
                    native_sha256=digest(output), original_run=manifest, native_command=command)
        report['cases'].append(case)
    write_exact(args.output / 'report.json', (json.dumps(report, indent=2) + '\n').encode())
    print('Map-effect construction/setters and450 template25 updates agree within stated scope')

if __name__ == '__main__':
    main()
