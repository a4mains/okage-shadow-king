#!/usr/bin/env python3
"""Compare material opacity/clamp helpers against pinned original EE execution.

The original helper executes 0x17e020 and 0x17dfe8. Its caller multiplications
are fixture preparation from static 0x17e058 recovery. This does not validate
normal lighting, VIF color-cache construction, texture sampling or rasterization.
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
    elf = Path('local/extracted/SCUS_971.29')
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    if sha(elf) != ELF_HASH:
        raise ValueError('Wrong retail ELF')
    transparency = [0., 1., .4, .41]
    for i in range(128):
        value = 1 - (i + .5) / 128
        word = struct.unpack('<I', struct.pack('<f', value))[0]
        transparency.extend(struct.unpack('<f', struct.pack('<I', word + delta))[0]
                            for delta in [-1, 0, 1])
    rng = random.Random(0x17e020)
    transparency.extend(rng.random() for _ in range(512))
    colors = [(1, .5, 0, 1), (-1, 2, .3, .5), (.5, .5, .5, 1), (0, 0, 0, 0)]
    data = b''.join(struct.pack('<5f', t, *c) for t in transparency for c in colors)
    cases = args.output/'cases.bin'
    write_exact(cases, data)
    oracle = Path('build/play-research/okage-oracle/okage-pose-oracle')
    original = args.output/'original.bin'
    commands = [[str(oracle), 'material-colors', str(elf), str(cases), str(original)]]
    subprocess.run(commands[0], check=True)
    expected = original.read_bytes()
    if len(expected) != len(data)//20*16:
        raise ValueError('Wrong original output extent')
    artifacts = [elf, cases, oracle, original]
    for build in args.build or ['native-release', 'native', 'model-sanitize']:
        binary = Path('build')/build/'okage-assets'
        output = args.output/(build+'.bin')
        command = [str(binary), 'material-colors', str(cases), str(output)]
        subprocess.run(command, check=True)
        commands.append(command)
        if output.read_bytes() != expected:
            raise ValueError('Material helper mismatch: '+build)
        artifacts.extend([binary, output])
    report = dict(scope=__doc__, elf_sha256=ELF_HASH, cases=len(data)//20,
                  exact_words=len(expected)//4, all_equal=True, commands=commands,
                  sha256={str(path): sha(path) for path in artifacts})
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print('Exact material helper cases:', report['cases'])

if __name__ == '__main__':
    main()
