#!/usr/bin/env python3
"""Compare effects90/105 against retained original state and105 packed UVs.

Native inputs are owned EFD/XIM plus explicit fixture transforms/RNG/camera.
This does not validate effect106, full drawing packets, raster or scene entry.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from verify_map_effect import compare, digest, ELF_SHA

CASES = {90: (1, 24, '90.input-v2.bin', '90.original-v2.bin'),
         105: (16, 180, '105.input.bin', '105.original-v1.bin')}
# Populated from the retained original executions and owned placement fixtures.
HASHES = {'90.input-v2.bin': '8cd9b7bb6d8a6942efe6cb436e0bebf49b4f58355d26eeaaa85120ec712cc49c', '90.original-v2.bin': 'e8f6796ed2f821719c43fb72e1d54b99624ea7ae0a9e3a93768e1212af5e0aed', '105.input.bin': 'a4a9cda3c0c8d9d825cbed671ff9ccb08cd27f6d2796034f0ae90ec12ece8082', '105.original-v1.bin': '16626754946ffebb0594284715e4eccf8b58aa92a743612817de98c06d8ebfa4', '105.original-v1.bin.uv.bin': '0c9da69d072a5b41798cb445e21e4078ea47f7bd031a1bf24818406a6688bdcb', '90.original-v2.bin.run.json': 'd7cbf012e0ca0dc3ab1c2984853c5285b6cc642faa89cf053fbbf0a33d90c4ed', '105.original-v1.bin.run.json': '8389b4cb53bdc62997e0adbe516167f5b58f50fe10cce0a8a202cbb1646cdc17'}

def compare_uv(original, native, frames=180):
    expected = []
    at = 0
    for step in range(frames + 2):
        for element in range(16):
            if at + 48 > len(original):
                raise ValueError('Incomplete original UV capture')
            row = struct.unpack_from('<12I', original, at)
            at += 48
            if row[:4] != (step, element, 0, 4) or any(row[i] for i in (5, 7, 9, 11)):
                raise ValueError('Original UV identity/count/tag changed')
            expected.extend((step, element, 4, row[4], row[6], row[8], row[10]))
    if at != len(original):
        raise ValueError('Trailing original UV observations')
    packed = struct.pack('<' + 'I' * len(expected), *expected)
    if native != packed:
        raise ValueError('Native packed UVs differ')
    return len(expected)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--reference', type=Path, default=Path('local/research/church-basement-effects-v1'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    if digest(Path('local/extracted/SCUS_971.29')) != ELF_SHA:
        raise ValueError('Different retail ELF')
    for name, expected in HASHES.items():
        if digest(args.reference / name) != expected:
            raise ValueError('Different retained input/output: ' + name)
    report = {'scope': __doc__.strip(), 'elf_sha256': ELF_SHA,
              'source_sha256': {p: digest(Path(p)) for p in
                                ('src/effect.cpp', 'src/assets_main.cpp', 'tools/verify_map_effect.py', __file__)},
              'reference_sha256': HASHES, 'cases': [],
              'owned_efd_sha256': digest(Path('local/effect-input/CMNDATA/2D/TES.EFD')),
              'owned_texture_sha256': {str(p): digest(p) for p in
                                      sorted(Path('local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF').glob('*.xim'))}}
    for template, (elements, frames, input_name, original_name) in CASES.items():
        original = args.reference / original_name
        original_run = json.loads(Path(str(original) + '.run.json').read_text())
        if original_run['exit_code']:
            raise ValueError('Incomplete original execution')
        recorded = original_run['command']
        if (len(recorded) != 7 or recorded[1] != 'map-effect-probe' or
                Path(recorded[4]).resolve() != (args.reference / input_name).resolve() or
                recorded[5] != str(frames) or Path(recorded[6]).resolve() != original.resolve() or
                original_run['input_sha256'] != digest(args.reference / input_name)):
            raise ValueError('Original manifest input/frame/output binding differs')
        output = args.output / f'{template}.native.bin'
        command = [str(args.native), 'map-effect-probe', 'local/effect-input/CMNDATA/2D/TES.EFD',
                   'local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF', str(args.reference / input_name),
                   str(frames), str(output)]
        result = subprocess.run(command, capture_output=True)
        (args.output / f'{template}.log').write_bytes(result.stdout + result.stderr)
        run = {'command': command, 'exit_code': result.returncode,
               'binary_sha256': digest(args.native)}
        (args.output / f'{template}.run.json').write_text(json.dumps(run, indent=2) + '\n')
        result.check_returncode()
        # The shared comparator permits tolerance only for25/19, never90/105.
        compare(original.read_bytes(), output.read_bytes(), template, elements, frames)
        case = {'template': template, 'records': frames + 2,
                'state_words_exact': (frames + 2) * (29 + 44 * elements),
                'original_run': original_run, 'native_run': run,
                'native_sha256': digest(output)}
        if template == 105:
            case['native_uv_sha256'] = digest(Path(str(output) + '.shadow-uv.bin'))
            case['uv_words_exact'] = compare_uv(Path(str(original) + '.uv.bin').read_bytes(),
                                                Path(str(output) + '.shadow-uv.bin').read_bytes())
        report['cases'].append(case)
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Effects90/105 state and105 UVs are exact; effect106/scene/raster remain unvalidated')

if __name__ == '__main__':
    main()
