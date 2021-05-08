#!/usr/bin/env python3
"""Compare native music-channel state with completed isolated EE/IOP probes.

This audits request/flush/cancel and buffered driver gains, not live RPC timing,
DMA, audible fidelity or whole-scene audio. Reference RAM is never native input.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from verify_event import digest, write_exact

EE_SHA = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
IOP_SHA = '7fe8b5a33c2f8700d1f964cf5ce2f66942f8f97d952e86e6dc0ad5d4f74becb2'
OUTPUTS = {'ee': '4483109f2378f81070012daf4e3d80d881ac8698582950c1ae9dd1c65c105c85',
           'iop': 'd4b835fab8adfa36cbd1d596ab93dde169782d60b5c4540433c52027ccdbc907'}

def fixture(kind):
    if kind == 'ee':
        rows = [(1,1,0,0), (0,0,7,128), (0,8,15,0), (1,1,0,0), (1,1,0,0),
                (0,2,4,64), (0,3,3,19), (1,0,0,0), (0,1,0,99), (1,1,0,0),
                (0,0,15,127), (2,0,0,0), (1,1,0,0), (0,15,15,0), (1,1,0,0),
                (0,0,0,1), (0,15,15,128), (1,1,0,0), (2,0,0,0), (1,0,0,0)]
    elif kind == 'iop':
        rows = [(active,channel,level,128-level) for active in (0,1)
                for channel in range(16) for level in (0,1,64,127,128)]
    else:
        raise ValueError('Unknown music-channel probe')
    return b''.join(struct.pack('<4I', *row) for row in rows)

def compare(original, native, kind):
    words = {'ee': 20*34, 'iop': 160*64}[kind]
    if len(original) != words*4 or len(native) != words*4:
        raise ValueError('Music-channel record extent differs')
    for i, (a, b) in enumerate(zip(struct.iter_unpack('<I', original),
                                  struct.iter_unpack('<I', native))):
        if a != b:
            raise ValueError(f'{kind} music-channel word {i}: {a[0]:08x} != {b[0]:08x}')
    return words

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--ee-reference', type=Path, default=Path('local/music-channel-probe-v1'))
    parser.add_argument('--iop-reference', type=Path, default=Path('local/music-channel-iop-v3'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--prepare', type=Path)
    args = parser.parse_args()
    if args.prepare:
        args.prepare.mkdir(parents=True, exist_ok=False)
        for kind in ('ee', 'iop'):
            write_exact(args.prepare/f'{kind}.input.bin', fixture(kind))
        return
    if not args.output:
        parser.error('--output is required')
    args.output.mkdir(parents=True, exist_ok=False)
    report = dict(scope=__doc__, native_binary_sha256=digest(args.native), probes={})
    for kind, root, asset, asset_sha, mode in (
        ('ee', args.ee_reference, Path('local/extracted/SCUS_971.29'), EE_SHA, 'music-channels'),
        ('iop', args.iop_reference, Path('local/extracted/MODULES/RXSNDDRV.IRX'), IOP_SHA, 'iop-music-channels')):
        source, original = root/f'{kind}.input.bin', root/f'{kind}.original.bin'
        run = json.loads((root/f'{kind}.run.json').read_text())
        if run['exit_code'] or run['asset_sha256'] != asset_sha or digest(asset) != asset_sha:
            raise ValueError('Incomplete or different original probe')
        if source.read_bytes() != fixture(kind) or run['input_sha256'] != digest(source):
            raise ValueError('Different music-channel fixture')
        if digest(original) != OUTPUTS[kind] or run['output_sha256'] != OUTPUTS[kind]:
            raise ValueError('Different reviewed original result')
        target = args.output/f'{kind}.native.bin'
        command = [str(args.native), mode, str(source), str(target)]
        subprocess.run(command, check=True)
        words = compare(original.read_bytes(), target.read_bytes(), kind)
        report['probes'][kind] = dict(original_run=run, command=command,
            exact_words=words, native_output_sha256=digest(target))
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print('20 EE operations and160 IOP cases match exactly (10920 words)')

if __name__ == '__main__':
    main()
