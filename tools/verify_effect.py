#!/usr/bin/env python3
"""Compare native type-30 effect animation with bounded original execution."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--reference', type=Path, help='Reuse completed original probe outputs')
    p.add_argument('--output', type=Path, default=Path('local/effect-validation'))
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    source = Path('local/effect-input/CMNDATA/2D/TES.EFD')
    ram = Path('local/text-validation-v4/opening600-ee.bin')
    report = {'scope': 'Type-30 allocation-derived element state and 300 animation updates; excludes attachment, rendering, scheduler behavior and whole-scene timing',
              'reference_preconditions': 'Captured EE RAM, retail VU0 microprogram upload, clock enabled. Original allocator/animation unpatched. Pinned Play BIOS handles an allowlist of queries and uncontended semaphore calls; rescheduling is rejected.',
              'efd_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'ram_sha256': hashlib.sha256(ram.read_bytes()).hexdigest(),
              'frames_per_case': 300, 'float_absolute_tolerance': 2e-6, 'cases': []}
    for parameter in (0, 1, 15, 16, 31):
        name = f'symbol-{parameter}.bin'
        reference = (a.reference or a.output)/name
        if not a.reference:
            subprocess.run(['build/play-research/okage-oracle/okage-pose-oracle', 'effect-probe',
                            'local/extracted/SCUS_971.29', str(ram), '30', str(parameter), '300', str(reference)], check=True)
        expected = reference.read_bytes()
        if len(expected) != 16+301*168:
            raise ValueError('Incomplete original effect probe')
        camera = a.output/f'camera-{parameter}.bin'
        write_exact(camera, expected[:16])
        output = a.output/f'native-{parameter}.bin'
        subprocess.run([str(a.native), 'effect-probe', str(source), str(camera), str(parameter), '300', str(output)], check=True)
        actual = output.read_bytes()
        if len(actual) != len(expected) or actual[:16] != expected[:16]:
            raise ValueError('Native effect probe extent/camera differs')
        maxima = [0.0]*42
        floats = set(range(2, 18)) | set(range(22, 38))
        for frame in range(301):
            at = 16+frame*168
            e, n = struct.unpack_from('<42I', expected, at), struct.unpack_from('<42I', actual, at)
            for i in range(42):
                if i not in floats:
                    if e[i] != n[i]:
                        raise RuntimeError(f'Symbol {parameter} frame {frame} word {i}: {e[i]:x} != {n[i]:x}')
                else:
                    x = struct.unpack('<f', struct.pack('<I', e[i]))[0]
                    y = struct.unpack('<f', struct.pack('<I', n[i]))[0]
                    delta = abs(x-y)
                    if not delta <= 2e-6:
                        raise RuntimeError(f'Symbol {parameter} frame {frame} float {i}: {x} != {y}')
                    maxima[i] = max(maxima[i], delta)
        report['cases'].append({'parameter': parameter, 'max_error_by_word': maxima,
                                'original_sha256': hashlib.sha256(expected).hexdigest(),
                                'native_sha256': hashlib.sha256(actual).hexdigest(),
                                'integer_words_exact': True})
    report['word_order'] = 'phase, flags; twice: position.xyz, rotation.xyz, scale.xyzw, color.rgba, damping, previous_time, program, logical_texture, uv_min_u16x2, uv_max_u16x2'
    write_exact(a.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print('Five symbols, initial state and 1500 effect updates agree within stated scope')

if __name__ == '__main__':
    main()
