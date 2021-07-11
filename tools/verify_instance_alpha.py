#!/usr/bin/env python3
"""Compare native instance alpha to original VU1 lighting and packed-color output.

Three directional lights have W=0; channel3 supplies ambient W. Original VU1
initialization, color upload, lighting and primitive emission execute unchanged.
This isolates alpha; it does not validate scene RGB lighting or final GS pixels.
"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
from prepare_assets import ELF_HASH
from verify_event import write_exact

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build', action='append', default=[])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    elf = Path('local/extracted/SCUS_971.29')
    if sha(elf) != ELF_HASH:
        raise ValueError('Wrong ELF')
    cases = []
    for source in [76., 77., 128., 255.]:
        for alpha in [-1., 0., .1, .5, .6, .99, 1., 2.]:
            cases.append((source, alpha))
        for value in range(1, 256):
            bits = struct.unpack('<I', struct.pack('<f', value/source))[0]
            for delta in [-1, 0, 1]:
                cases.append((source, struct.unpack('<f', struct.pack('<I', bits+delta))[0]))
    rng = random.Random(0x1b7bb8)
    cases.extend((rng.uniform(0, 255), rng.uniform(-1, 2)) for _ in range(512))
    data = b''.join(struct.pack('<2f', *r) for r in cases)
    inputs = args.output/'cases.bin'
    write_exact(inputs, data)
    oracle = Path('build/play-research/okage-oracle/okage-pose-oracle')
    original = args.output/'original.bin'
    commands = [[str(oracle), 'instance-alpha', str(elf), str(inputs), str(original)]]
    subprocess.run(commands[0], check=True)
    records = original.read_bytes()
    if len(records) != len(data):
        raise ValueError('Original output extent')
    expected = bytearray()
    for at in range(0, len(data), 8):
        _, ambient = struct.unpack_from('<2f', data, at)
        light, packed = struct.unpack_from('<fI', records, at)
        if light != max(0., ambient) or packed & 0xffffff00 != 0x4b000000:
            raise ValueError(f'Original lighting/packing contract at record{at//8}')
        expected += struct.pack('<f', packed & 255)
    artifacts = [elf, oracle, inputs, original]
    for build in args.build or ['native-release', 'native', 'model-sanitize']:
        binary = Path('build')/build/'okage-assets'
        output = args.output/(build+'.bin')
        command = [str(binary), 'instance-alpha', str(inputs), str(output)]
        subprocess.run(command, check=True)
        commands.append(command)
        if output.read_bytes() != expected:
            raise ValueError('Native alpha mismatch: '+build)
        artifacts.extend([binary, output])
    report = dict(scope=__doc__, elf_sha256=ELF_HASH, cases=len(cases), all_equal=True,
                  commands=commands, sha256={str(p): sha(p) for p in artifacts})
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print('Exact original VU1/native alpha cases:', len(cases))

if __name__ == '__main__':
    main()
