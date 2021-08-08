#!/usr/bin/env python3
"""Audit native summoning effect state and mutable ring coordinates.

Uses completed isolated original probes. Covers constructors, full-lifetime
element updates, RNG, program changes and removal requests for33/115, plus
shape25's thirty mutable Y coordinates. Does not validate other geometry,
materials, transforms into camera space, deferred scheduling or raster output.
Reference RAM is never an input to the native executable.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from verify_event import write_exact
from verify_map_effect import ELF_SHA, compare, digest

CASES = {
    33: (9, 241, '21473d041b24a853bf13ceaeb69d6a5f15fa383e807bde7e8e6730d1798be635',
         'ab8430907a7d0cf47c2367c762f549be22ca2aff3e982ceac159c178650a5e28'),
    115: (5, 501, '55f948cbe97b6c1746ab3d4e32f3ecaebf6ac7bcc376773b51722216b9f7b0fb',
          'a14ae794911b53dfa18fac872ed170215a3e6db54e2cda5be2fc0b6840d6c05d'),
}
GEOMETRY_SHA = 'f97fa5914ba4b87882f4e2950abecfae0c7847c93e5e01efe52d8f985ce04e0f'
# Complete original geometry record layout; only element1's tag2 Y is compared.
LAYOUT = ((0, 0, 17), (1, 0, 60), (2, 0, 95), (2, 1, 20),
          *((element, 0, 4) for element in range(3, 9)))

def compare_ring(original, native, frames=241):
    if len(native) != (frames + 2) * 33 * 4:
        raise ValueError('Incomplete native ring interval')
    at = 0
    for step, got in enumerate(struct.iter_unpack('<33I', native)):
        for element, piece, count in LAYOUT:
            if len(original) - at < 16 + count * 12:
                raise ValueError('Incomplete original geometry interval')
            if struct.unpack_from('<4I', original, at) != (step, element, piece, count):
                raise ValueError('Original geometry identity/order differs')
            at += 16
            if element == 1:
                points = list(struct.iter_unpack('<3I', original[at:at + count * 12]))
                if [p[2] for p in points] != [1, 2] * 30:
                    raise ValueError('Original ring vertex tags differ')
                expected = (step, 1, 30, *(p[0] >> 16 for p in points[1::2]))
                if got != expected:
                    raise ValueError('Native ring state/coordinate differs')
            at += count * 12
    if at != len(original):
        raise ValueError('Trailing original geometry observations')
    return {'records': frames + 2, 'mutable_y_words_exact': (frames + 2) * 30}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--reference', type=Path, default=Path('local/research/basement-summon-v1'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    if digest(Path('local/extracted/SCUS_971.29')) != ELF_SHA:
        raise ValueError('Different retail ELF')
    efd = Path('local/effect-input/CMNDATA/2D/TES.EFD')
    textures = Path('local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF')
    report = {'scope': __doc__, 'elf_sha256': ELF_SHA, 'efd_sha256': digest(efd),
              'native_sha256': digest(args.native), 'cases': []}
    for template, (elements, frames, input_hash, output_hash) in CASES.items():
        fixture = args.reference / f'{template}.input.bin'
        original = args.reference / f'{template}.original.bin'
        run = json.loads((args.reference / f'{template}.run.json').read_text())
        if run['exit_code'] or digest(fixture) != input_hash or digest(original) != output_hash:
            raise ValueError('Incomplete or changed original probe')
        service_run = json.loads((args.reference / f'{template}.service.run.json').read_text())
        if service_run['exit_code'] or digest(args.reference / f'{template}.service-original.bin') != output_hash:
            raise ValueError('Incomplete or changed positioned-service probe')
        output = args.output / f'{template}.native.bin'
        command = [str(args.native), 'map-effect-probe', str(efd), str(textures),
                   str(fixture), str(frames), str(output)]
        result = subprocess.run(command, capture_output=True)
        write_exact(args.output / f'{template}.log', result.stdout + result.stderr)
        write_exact(args.output / f'{template}.run.json',
                    (json.dumps({'command': command, 'exit_code': result.returncode,
                                 'binary_sha256': digest(args.native)}, indent=2) + '\n').encode())
        result.check_returncode()
        compare(original.read_bytes(), output.read_bytes(), template, elements, frames)
        if original.read_bytes() != output.read_bytes():
            raise ValueError('Summoning state is not bit exact')
        case = {'template': template, 'frames': frames, 'records': frames + 2,
                'words_exact': (frames + 2) * (29 + 44 * elements),
                'input_sha256': input_hash, 'state_sha256': output_hash,
                'original_run': run, 'original_service_run': service_run, 'native_command': command}
        if template == 33:
            geometry = args.reference / '33.geometry-original.bin.geometry.bin'
            geometry_run = json.loads((args.reference / '33.geometry.run.json').read_text())
            if geometry_run['exit_code'] or digest(geometry) != GEOMETRY_SHA:
                raise ValueError('Incomplete or changed original geometry probe')
            deformation = Path(str(output) + '.deformation.bin')
            case['ring'] = compare_ring(geometry.read_bytes(), deformation.read_bytes(), frames)
            case['ring'].update(original_sha256=GEOMETRY_SHA,
                                native_sha256=digest(deformation), original_run=geometry_run)
        report['cases'].append(case)
    write_exact(args.output / 'report.json', (json.dumps(report, indent=2) + '\n').encode())
    print(json.dumps({'templates': list(CASES), 'all_state_words_and_ring_y_exact': True}))

if __name__ == '__main__':
    main()
