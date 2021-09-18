#!/usr/bin/env python3
"""Verify STUN's scale-aware motion against original EE/VU execution.

The optional native route audit checks phase0 follow and hidden lifecycle only.
It is not a whole-game original comparison or service54/projection proof.
"""
import argparse
from fractions import Fraction
import json
from pathlib import Path
import struct
import subprocess
import time
from verify_animation import compare
from verify_event import write_exact
from verify_map_effect import digest, ELF_SHA

def subtract_toward_zero(word, amount):
    value = struct.unpack('<f', struct.pack('<I', word))[0]
    exact = Fraction(value) - Fraction(amount)
    bits = struct.unpack('<I', struct.pack('<f', float(exact)))[0]
    rounded = struct.unpack('<f', struct.pack('<I', bits))[0]
    if abs(Fraction(rounded)) > abs(exact):
        bits -= 1
    return bits

def audit_follow(data):
    if not data or len(data) % 120:
        raise ValueError('STUN follow record extent')
    amount = struct.unpack('<f', struct.pack('<f', .01))[0]
    frozen = 0
    for frame, row in enumerate(struct.iter_unpack('<30I', data), 1):
        if row[0] != frame or row[1] not in (0, 1):
            raise ValueError('STUN follow frame/movement-mask scope')
        if row[2:10] != (0, 0, 0, 0xffffffff, 0, 4, 0, 0x3f800000):
            raise ValueError('STUN ordinary hidden state changed')
        expected = list(row[26:30])
        expected[1] = subtract_toward_zero(expected[1], amount)
        if row[10:14] != row[22:26] or row[14:18] != tuple(expected):
            raise ValueError(f'STUN follow pose differs at frame{frame}')
        if row[18:22] != (0x3f800000,) * 4:
            raise ValueError('STUN phase0 scale changed')
        frozen += row[1] & 1
    return dict(frames=frame, movement_frozen_follow_frames=frozen)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, required=True)
    p.add_argument('--oracle', type=Path, default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference', type=Path, default=Path('local/research/stun-integration-v1'))
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--capture', type=Path)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    a.reference.mkdir(parents=True, exist_ok=True)
    assets = Path('local/xpf-decoded/FLDDATA/OBJ/STUN.XPF')
    elf = Path('local/extracted/SCUS_971.29')
    if digest(elf) != ELF_SHA:
        raise ValueError('Different retail ELF')
    original = a.reference / 'motion1-original-v2.bin'
    manifest = a.reference / 'motion1-original-v2.run.json'
    inputs = {str(path): digest(path) for path in [elf, assets/'a.xmt', assets/'a.xmd', assets/'a.xta']}
    if not original.exists():
        if manifest.exists():
            raise ValueError('Retained original failure; investigate before a fresh capture')
        cmd = [str(a.oracle), 'animation', str(elf), str(assets/'a.xmt'), '196', '1', '180', str(original), str(assets/'a.xmd'), str(assets/'a.xta')]
        start = time.monotonic()
        with (a.reference/'motion1-original-v2.log').open('w') as log:
            result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
        run = dict(command=cmd, exit_code=result.returncode, elapsed_seconds=time.monotonic()-start,
                   binary_sha256=digest(a.oracle), inputs=inputs)
        if not result.returncode:
            run['output_sha256'] = digest(original)
        write_exact(manifest, (json.dumps(run, indent=2)+'\n').encode())
        if result.returncode:
            raise RuntimeError('Original STUN motion failed; retained log/manifest')
    run = json.loads(manifest.read_text())
    if run['exit_code'] or run['inputs'] != inputs or run['output_sha256'] != digest(original):
        raise ValueError('Incomplete/changed original STUN motion')
    native = a.output/'motion1-native.bin'
    cmd = [str(a.native), 'animation', str(assets/'a.xmt'), '196', '1', '180', str(native), str(assets/'a.xmd'), str(assets/'a.xta')]
    subprocess.run(cmd, check=True)
    result = compare(original, native, 196, 64, 180, 3)
    result['dump_bytes_exact'] = original.read_bytes() == native.read_bytes()
    result['passed'] = result['passed'] and result['dump_bytes_exact']
    report = dict(scope=__doc__, original_run=run, native_command=cmd,
                  native_sha256=digest(a.native), comparison=result)
    if a.capture:
        path = Path(str(a.capture)+'.shadow.bin')
        route = json.loads(Path(str(a.capture).removesuffix('.png')+'.run.json').read_text())
        if route['exit_code']:
            raise ValueError('Incomplete native route')
        report['follow'] = dict(audit_follow(path.read_bytes()), path=str(path), sha256=digest(path), run=route)
    write_exact(a.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    if not result['passed']:
        raise ValueError('STUN original motion mismatch; retained comparison report')
    print(json.dumps(result))

if __name__ == '__main__':
    main()
