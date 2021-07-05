#!/usr/bin/env python3
"""Import reviewed ordinary music resources from the owned USA disc.

The ELF name table and disc hashes are checked before installing owned inputs.
This is resource provenance, not a music playback or original PCM comparison.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from inspect_disc import Disc, inspect_elf
from prepare_assets import ELF_HASH
from verify_event import write_exact

RESOURCES = {
    38: ('EVN_THL', {
        'EVN_THL.XSQ': 'ca285623d8930060e39ffa3b02b292589b5550c58e9d85859b382e0afea89b85',
        'EVN_THL0.XV': '9dc31ed9239b5147d88807fa913a87019b0d5b4a2d498426e4d6d176826f2662',
        'EVN_THL1.XV': 'c1768b23a7d7a8b678eb6aa54c1fff903343fd25a3947ecf0abde4f88811b0e0',
    }),
    50: ('EVS_NRD', {
        'EVS_NRD.XSQ': 'c255165d2dd93ff4eb82a918ff30d11cf9d5011d27a10bd8127c07c077fb7594',
        'EVS_NRD0.XV': '9367e1eb1cd95a5d79cc02984daf121c64dc4dd48b70f5c8b64bf7922c0f5a29',
    }),
    54: ('EVS_MPS', {
        'EVS_MPS.XSQ': 'f49b77cc68c6e7a0f9c3b6ebc588e6e19b5729889faa5bc68caae53823c7aa37',
        'EVS_MPS0.XV': 'cd6b32d56e0a60d3e8732e4b132c512aaf04b8ea24067691aa33d9f85a1adada',
    }),
}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--id', type=int, choices=RESOURCES, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    elf = Path('local/extracted/SCUS_971.29').read_bytes()
    if hashlib.sha256(elf).hexdigest() != ELF_HASH:
        raise ValueError('Different owned ELF')
    segments = inspect_elf(elf)['segments']

    def address(at, size):
        for s in segments:
            base = int(s['vaddr'], 16)
            if s['type'] == 1 and base <= at and at + size <= base + s['filesz']:
                offset = s['offset'] + at - base
                return elf[offset:offset + size]
        raise ValueError('ELF address outside file-backed load segments')

    name, hashes = RESOURCES[a.id]
    table = 0x1fb530 + a.id * 8
    name_pointer, class_pointer = struct.unpack('<2I', address(table, 8))
    if address(name_pointer, len(name) + 1) != name.lower().encode() + b'\0':
        raise ValueError('Music name table changed')
    if address(class_pointer, 2) != b'1\0':
        raise ValueError('Music class differs from reviewed ordinary sequence route')
    entries = {e['path']: e for e in json.loads(Path('docs/disc-report.json').read_text())['files']}
    second = name + '1.XV'
    if ('SNDDATA/BGM/' + second in entries) != (second in hashes):
        raise ValueError('Secondary bank differs from reviewed resources')
    disc = Disc('local/disc/Okage - Shadow King (USA).bin')
    imported = []
    try:
        for filename, expected in hashes.items():
            source = 'SNDDATA/BGM/' + filename
            entry = entries[source]
            data = disc.read(entry['lba'], entry['size'])
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError('Different owned music resource: ' + source)
            target = Path('local/audio-input') / source
            target.parent.mkdir(parents=True, exist_ok=True)
            write_exact(target, data)
            imported.append(dict(path=str(target), lba=entry['lba'], size=entry['size'], sha256=expected))
    finally:
        disc.close()
    report = dict(scope=__doc__, elf_sha256=ELF_HASH, music_id=a.id, table_address=table,
                  name_pointer=name_pointer, class_pointer=class_pointer, sequence=name,
                  primary_bank=name + '0', secondary_bank_present=second in hashes, imported=imported)
    write_exact(a.output, (json.dumps(report, indent=2) + '\n').encode())
    print('Imported reviewed music', a.id, name)

if __name__ == '__main__':
    main()
