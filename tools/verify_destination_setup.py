#!/usr/bin/env python3
"""Audit reached destination setup and projection, not completed outdoor startup.

Reads an existing native route log and completed original RAM for comparison.
Only the projection subprocess executes native code here; it receives owned STG
data, never RAM. The route log does not prove rendering, audio PCM or controls.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
from prepare_assets import ELF_HASH
from verify_event import write_exact

def compare(log, ram, projection):
    if len(ram) != 32*1024*1024 or len(projection) != 16:
        raise ValueError('Reference RAM/projection extent')
    word = lambda at: struct.unpack_from('<I', ram, at)[0]
    signed = lambda at: struct.unpack_from('<i', ram, at)[0]
    field = word(0x2d9bf4)
    if not 0x100000 <= field <= len(ram)-0x800:
        raise ValueError('Original field pointer bounds')
    game = word(field+0x14)
    if not 0x100000 <= game <= len(ram)-0x60:
        raise ValueError('Original game pointer bounds')
    if (word(game+0x44), word(game+0x4c), word(field+0x410)) != (1, 1, 60):
        raise ValueError('Expected original scene1 entrance1 startup60')
    pattern = (r'^Native destination setup: scene (-?\d+); party (\d+); '
               r'followers (\d+); music (-?\d+); slots (-?\d+),(-?\d+)$')
    reached = [tuple(map(int, m)) for m in re.findall(pattern, log, re.MULTILINE)]
    if len(reached) != 2 or reached[0] != (70, 0, 0, 51, 51, -1):
        raise ValueError('Missing/unexpected native interior-to-exterior setup sequence')
    expected = (word(game+0x44), word(field+0x80), word(field+0x7c),
                signed(0x2d9ab4), signed(0x2d9aa8), signed(0x2d9aac))
    if reached[1] != expected:
        raise ValueError('Native destination party/roster/music mismatch')
    expected_projection = b''.join(ram[0x332220+i*4:0x332224+i*4] for i in (0, 1, 2, 6))
    if projection != expected_projection:
        raise ValueError('Native projection mismatch')
    return {'scene_party_followers_playing_slot0_slot1': list(expected),
            'projection_words': list(struct.unpack('<4I', projection)),
            'all_compared_words_exact': True, 'compared_words': 10}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--reference-ram', type=Path, default=Path('local/research/exterior-load180-v1-ee.bin'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    elf = Path('local/extracted/SCUS_971.29')
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    if sha(elf) != ELF_HASH:
        raise ValueError('Wrong retail ELF')
    stage = Path('local/xpf-decoded/FLDDATA/MAP/FIELD/FIELD_1D.XPF/a.stg')
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output/'projection.bin'
    if output.exists():
        raise ValueError('Use a fresh audit output')
    command = [str(args.native), 'field-projection', str(stage), str(output)]
    subprocess.run(command, check=True)
    report = compare(args.log.read_text(), args.reference_ram.read_bytes(), output.read_bytes())
    report.update({
        'scope': 'Reached native destination setup identity and STG projection; not startup completion or playability',
        'elf_sha256': ELF_HASH, 'projection_command': command,
        'inputs': [{'path': str(p), 'sha256': sha(p)} for p in (args.log, args.native, stage, args.reference_ram)],
        'limits': ['Native log checks six reported setup values, not all constructor state or resource lifetimes',
                   'Reference was captured later during startup; only stable setup values are compared',
                   'No loading latency, music switch waveform, rendering, startup completion or controls comparison',
                   'This auditor does not launch the route; retain its command, exit status and binary provenance separately'],
    })
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print('Destination setup and four projection words match; outdoor playability is not established')

if __name__ == '__main__':
    main()
