#!/usr/bin/env python3
"""Bounded SHADOW handler/controller comparison using an original research snapshot.

Original dispatch covers the field/battle-mode gate and transition request switch.
Accepted isolated transitions use the original mute bit; audio RPC needs the full VM.
Unmuted requests cover ignored transitions only; the integrated route tests real audio dispatch.
Native comparison covers command forwarding, consumption and controller timing;
shared field effects/transition integration has separate field tests and route evidence.
The research snapshot never enters the native runtime.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from prepare_assets import ELF_HASH
from verify_event import write_exact

def fixtures():
    records = []
    for request in [0, 1, 2, 3, 4, 5, 6, 128, 129, 256, 257, 384, 385, 0xffffffff]:
        for lock in [0, 1]:
            for mode in [2, 4]:
                for delay in [0, 7, 0xffffffff]:
                    phase = 0 if request & 127 == 1 else 3
                    if mode == 2 and not lock and request & 127 <= 1 and not request & 128:
                        phase = 1
                    records.append((len(records), request, phase, lock, 1, delay, 0x8910, mode))
    # Requests made during intermediate transitions must be ignored.
    for phase in [1, 2, 4, 5]:
        for request in [0, 1]:
            records.append((len(records), request, phase, 0, 1, 7, 0x8910, 2))
    return b''.join(struct.pack('<8I', *r) for r in records)

def rows(data, words):
    if not data or len(data) % (words * 4):
        raise ValueError('Dialogue shadow record extent')
    return list(struct.iter_unpack('<' + 'I' * words, data))

def _reported_sha256(hashes, path):
    """Return the uniquely reported hash for a path, resolving relative entries."""
    target = path.resolve()
    matches = [digest for name, digest in hashes.items()
               if (Path(name) if Path(name).is_absolute() else Path.cwd() / name).resolve() == target]
    if len(matches) != 1 or not isinstance(matches[0], str) or len(matches[0]) != 64:
        raise ValueError(f'Missing or ambiguous source hash for {path}')
    return matches[0]

def validate_original_provenance(original, inputs, elf, ram):
    """Validate a retained oracle output against its adjacent report and inputs.

    The producer binary is identified by the hash recorded when the original
    report was made. It is deliberately not compared with a currently installed
    oracle binary, which may have changed since that retained run.
    """
    report_path = original.parent / 'report.json'
    try:
        report_bytes = report_path.read_bytes()
        report = json.loads(report_bytes)
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f'Cannot read adjacent original report {report_path}: {exc}') from exc
    hashes = report.get('sha256')
    commands = report.get('commands')
    if not isinstance(hashes, dict) or not isinstance(commands, list):
        raise ValueError('Original report lacks hashes or commands')

    for path in (original, original.parent / 'cases.bin', elf, ram):
        expected = _reported_sha256(hashes, path)
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f'Original report hash mismatch for {path}')
    if (original.parent / 'cases.bin').read_bytes() != inputs.read_bytes():
        raise ValueError('Retained fixture mismatch')

    producers = []
    for command in commands:
        if (isinstance(command, list) and len(command) == 6 and
                command[1] == 'dialogue-shadow' and
                Path(command[2]).resolve() == elf.resolve() and
                Path(command[3]).resolve() == ram.resolve() and
                Path(command[4]).resolve() == (original.parent / 'cases.bin').resolve() and
                Path(command[5]).resolve() == original.resolve()):
            producers.append(command)
    if len(producers) != 1:
        raise ValueError('Original report lacks a unique matching oracle producer command')
    producer = producers[0]
    producer_hash = _reported_sha256(hashes, Path(producer[0]))
    return {
        'report': str(report_path),
        'report_sha256': hashlib.sha256(report_bytes).hexdigest(),
        'original_output_sha256': _reported_sha256(hashes, original),
        'producer': {'executable': producer[0], 'sha256': producer_hash},
    }

def compare(inputs, original, native):
    cases, expected, actual = rows(inputs, 8), rows(original, 20), rows(native, 14)
    if len(expected) != len(cases) * 2 or len(actual) != len(cases):
        raise ValueError('Dialogue shadow output count')
    for f, handler, controller, n in zip(cases, expected[::2], expected[1::2], actual):
        ident, request, phase, lock, follow, delay, flags, mode = f
        state = [phase, lock, follow, 0xffffffff, 0x3ec00000, 9]
        action = request & 127
        if mode != 4:
            if action <= 1 and not lock and phase == (0 if action else 3):
                state[0], state[4], state[5] = (1 if action else 4), 0x3f800000, action
            elif action == 2:
                state[2] = 0
            elif action == 3:
                state[2] = 1
            elif action == 4:
                state[1] = 0
            elif action == 5:
                state[1] = 1
        if handler[:3] != (ident, 0, 0xffffffff) or controller[:3] != (ident, 1, 0):
            raise ValueError(f'Original handler return/identity differs at case {ident}')
        if handler[3:9] != tuple(state) or controller[3:9] != tuple(state):
            raise ValueError(f'Original shadow dispatch differs at case {ident}')
        text = (flags, 9, 0xfffffffe, delay, 5, 4, 0x50, 5, 255, 1, mode)
        if handler[9:] != text or controller[9:] != text:
            raise ValueError(f'Original text timing differs at case {ident}')
        if n[:4] != (ident, 1, request, 0) or n[4:] != controller[9:19]:
            raise ValueError(f'Native controller differs at case {ident}')
    return dict(cases=len(cases), original_dispatch_records=len(expected),
                exact_native_controller_words=len(cases)*10, native_field_side_effect_comparison=False)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ram', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--native', type=Path, default=Path('build/native-release/okage-assets'))
    p.add_argument('--original', type=Path, help='Reuse output only with a matching adjacent report.json')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    elf = Path('local/extracted/SCUS_971.29')
    if sha(elf) != ELF_HASH or a.ram.stat().st_size != 32*1024*1024:
        raise ValueError('Wrong ELF/snapshot extent')
    inputs = a.output/'cases.bin'; write_exact(inputs, fixtures())
    commands = []; artifacts = [elf, a.ram, inputs, a.native]
    original_provenance = None
    if a.original:
        original = a.original
        original_provenance = validate_original_provenance(original, inputs, elf, a.ram)
    else:
        original = a.output/'original.bin'
        oracle = Path('build/play-research/okage-oracle/okage-pose-oracle')
        cmd = [str(oracle), 'dialogue-shadow', str(elf), str(a.ram), str(inputs), str(original)]
        commands.append(cmd); subprocess.run(cmd, check=True); artifacts.append(oracle)
    native = a.output/'native.bin'
    cmd = [str(a.native), 'dialogue-shadow', str(inputs), str(native)]
    commands.append(cmd); subprocess.run(cmd, check=True)
    result = compare(inputs.read_bytes(), original.read_bytes(), native.read_bytes())
    artifacts += [original, native]
    report = dict(scope=__doc__, result=result, commands=commands,
                  sha256={str(f): sha(f) for f in artifacts})
    if original_provenance is not None:
        report['original_provenance'] = original_provenance
    write_exact(a.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(json.dumps(result))

if __name__ == '__main__':
    main()
