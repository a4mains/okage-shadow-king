#!/usr/bin/env python3
"""Replay the captured FIELD_1D door requests against native owned-asset motion.

This bounded audit does not validate full event60 execution or scene rendering.
Reference traces are auditor inputs only; the shipping runtime never reads them.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from verify_event import write_exact

ELF_SHA = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
FIELD = 0x133a98
REQUEST = 0x1b7a68

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def rows(data, words):
    if not data or len(data) % (words * 4):
        raise ValueError('Empty or truncated motion trace')
    return list(struct.iter_unpack('<' + 'I' * words, data))

def reference(first, continuation):
    """Check the completed 260+60-vblank fixture and derive request timing."""
    samples = []
    requests = []
    for data, start, count, offset in ((first, 140, 120, 0), (continuation, 0, 60, 120)):
        records = rows(data, 48)
        previous = -1
        fields = []
        for row in records:
            if row[0] < previous or row[4:6] != (18, 10):
                raise ValueError('Reference order/instance/channel mismatch')
            previous = row[0]
            if row[7] == FIELD:
                if row[6] != 0:
                    raise ValueError('Field sample contains a motion request')
                fields.append(row)
            elif row[7] == REQUEST:
                requests.append((offset + row[0] - start + 1, row[6]))
            else:
                raise ValueError('Unknown reference observation boundary')
        if [r[0] for r in fields] != list(range(start, start + count)):
            raise ValueError('Incomplete or shifted reference field interval')
        samples.extend(fields)
    if requests != [(0, 1), (84, 3)]:
        raise ValueError('Door request timing differs from completed fixture')
    return samples, requests

def compare(samples, native):
    actual = rows(native, 46)
    if len(actual) != len(samples):
        raise ValueError('Native motion interval length mismatch')
    for index, (ref, got) in enumerate(zip(samples, actual)):
        if got[0] != index or got[3:6] != (1, 18, 10):
            raise ValueError(f'Native frame/visibility/binding mismatch at {index}')
        if ref[2:4] != got[1:3] or ref[8:] != got[6:]:
            raise ValueError(f'Motion script/channel mismatch at {index}')
    if len({r[6:] for r in actual[-10:]}) != 1:
        raise ValueError('Door has not settled through the final ten updates')
    return len(actual)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('local/map-motion-validation'))
    args = parser.parse_args()
    elf = Path('local/extracted/SCUS_971.29')
    if digest(elf) != ELF_SHA:
        raise ValueError('Retail ELF identity mismatch')
    prefixes = [Path('local/reference-outdoor-door-260-v2'),
                Path('local/reference-outdoor-door-close-60-v1')]
    artifacts = [elf]
    for prefix, duration, source in zip(prefixes, (260, 60),
                                      ('local/reference-front-door-180-v1.state.zip',
                                       str(prefixes[0]) + '.state.zip')):
        log = Path(str(prefix) + '.log')
        contents = log.read_text()
        if (f'Loaded reference state {source}' not in contents or
                f'Stopped: vblanks={duration} exit_request=0 ' not in contents or
                'Captured rendered reference layer 0: 640x224' not in contents):
            raise ValueError('Capture completion/save-chain provenance mismatch')
        artifacts.extend([log, Path(source), Path(str(prefix) + '.state.zip'),
                          Path(str(prefix) + '.actor.bin')])
    samples, requests = reference(*(Path(str(p) + '.actor.bin').read_bytes() for p in prefixes))
    args.output.mkdir(parents=True, exist_ok=True)
    replay = args.output / 'requests.input'
    write_exact(replay, ''.join(f'{frame} {entry}\n' for frame, entry in requests).encode())
    game_map = Path('local/xpf-decoded/FLDDATA/MAP/FIELD/FIELD_1D.XPF/a.xmp')
    artifacts.extend([game_map, game_map.with_name('fh_dor00.xmd'),
                      game_map.with_name('fh_dor00.xmt'), replay])
    commands = []
    results = []
    baseline = None
    for build in ('native', 'native-release', 'model-sanitize'):
        binary = Path('build') / build / 'okage-assets'
        output = args.output / (build + '.bin')
        if output.exists() or output.is_symlink():
            raise ValueError(f'Choose a fresh output directory: {output}')
        command = [str(binary), 'map-motion', str(game_map), '18', str(replay), '180', '10', str(output)]
        subprocess.run(command, check=True)
        data = output.read_bytes()
        updates = compare(samples, data)
        if baseline is not None and data != baseline:
            raise ValueError('Native builds produce different motion output')
        baseline = data
        commands.append(command)
        artifacts.extend([binary, output])
        results.append({'build': build, 'exact_updates': updates})
    report = {
        'scope': "Isolated native door motion driven by original motion-selection requests.",
        'elf_sha256': ELF_SHA,
        'reference_prefixes': [str(p) for p in prefixes],
        'instance': 18, 'channels': 10, 'exact_channel_words_per_update': 40,
        'exact_motion_pc_and_script_state': True,
        'request_replay': requests, 'continuation_native_frame': 120,
        'timing': 'Previous-render channels at field entry; request frame minus first sample plus one.',
        'builds': results, 'native_build_outputs_identical': True,
        'final_ten_channel_states_stable': True,
        'commands': commands,
        'limits': [
            'Does not validate full native event60, outdoor rendering, NPCs, camera or audio.',
            'Observed requests drive the isolated replay; they are not native runtime inputs.',
            'The earlier incomplete draw-boundary v1 capture is excluded.',
        ],
        'sha256': {str(p): digest(p) for p in artifacts},
    }
    write_exact(args.output / 'report.json', (json.dumps(report, indent=2) + '\n').encode())
    print(f'Validated {len(samples)} updates in three native builds: {args.output / "report.json"}')

if __name__ == '__main__':
    main()
