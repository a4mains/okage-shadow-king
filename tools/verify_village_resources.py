#!/usr/bin/env python3
"""Audit VIL selection/name binding and reproducibly import village music2.

Uses completed original message probes with actual message construction. Does not
establish bank-switch lifetime, dialogue timing, music PCM fidelity or startup.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from inspect_disc import Disc
from verify_event import digest, write_exact
from verify_village_effect import RAM_SHA
from verify_map_effect import ELF_SHA

MUSIC = {
    'CTY_TNB.XSQ': '81626d50d22a3b2ec5e538a5b2b4788604116a15e43091ad0d08ab42f72adfa5',
    'CTY_TNB0.XV': '9c1717b52831745025fdaef4e98a7d3d44e4728e48503399a79d7bd9f98f791b',
    'CTY_TNB1.XV': '0947f58340ca4a719dc20079f240d61c19a9e2b27254387e1231d54da7275e03',
}
MESSAGES = {
    'pme-indices-10004': '55772ffe2d4083457a70d11d991908e8cdfee00673cdd2bc962fcc0105bb303a',
    'pme-indices-10013': '864ca238596875923d59ec5d3c50ae87973bb25aaa5fff6b503c87e2892a7e5f',
    'pme-indices-10205': 'aed8a339938dd3cc5dc7c8b5699cd8086dd46b2d371309928b436c55182af310',
    'pme-name-10004': '91282e23685e64f9dbad52848e6e6f10f037a3bc6dfdf75d64b80b4fe99c4b99',
    'pme-name-10013': 'f2992997d5574a268ad678a504ee79175188fe6d225639a4c7100b3632a787b0',
    'pme-name-10205': '1731041c01d946ab88aadae0dfb62263a82d29880581527313a69c2f463e9785',
}

def prepare_music():
    entries = {e['path']: e for e in json.loads(Path('docs/disc-report.json').read_text())['files']}
    disc = Disc('local/disc/Okage - Shadow King (USA).bin')
    imported = []
    try:
        for name, expected in MUSIC.items():
            source = 'SNDDATA/BGM/'+name
            entry = entries[source]
            data = disc.read(entry['lba'], entry['size'])
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError('Different owned village music resource: '+source)
            target = Path('local/audio-input')/source
            target.parent.mkdir(parents=True, exist_ok=True)
            write_exact(target, data)
            imported.append(dict(path=str(target), sha256=expected, lba=entry['lba'], size=entry['size']))
    finally:
        disc.close()
    return imported

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--reference', type=Path, default=Path('local/village-bank-probe-v2'))
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--prepare', action='store_true', help='Import music only; retain an import report')
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    if args.prepare:
        write_exact(args.output/'import.json', (json.dumps(prepare_music(), indent=2)+'\n').encode())
        return
    ram = Path('local/research/village300-v1-ee.bin')
    if digest(ram) != RAM_SHA or digest(Path('local/extracted/SCUS_971.29')) != ELF_SHA:
        raise ValueError('Different original RAM/ELF')
    memory = ram.read_bytes()
    pointer, = struct.unpack_from('<I', memory, 0x1fb530+2*8)
    if pointer != 0x227be0 or memory[pointer:pointer+8] != b'cty_tnb\0':
        raise ValueError('Original music2 mapping changed')
    for name, expected in MUSIC.items():
        if digest(Path('local/audio-input/SNDDATA/BGM')/name) != expected:
            raise ValueError('Music resource changed')
    bank = Path('local/text-input/FLDDATA/MES/VIL.PME')
    report = dict(scope=__doc__, elf_sha256=ELF_SHA, ram_sha256=RAM_SHA, bank_sha256=digest(bank),
                  native_binary_sha256=digest(args.native), music2=MUSIC, cases=[])
    for name, expected in MESSAGES.items():
        mode, message = name.rsplit('-', 1)
        original = args.reference/(name+'.original.bin')
        run = json.loads((args.reference/(name+'.run.json')).read_text())
        if run['exit_code'] or run['elf_sha256'] != ELF_SHA or run['ram_sha256'] != RAM_SHA:
            raise ValueError('Incomplete or different original message probe')
        if digest(original) != expected or expected != run['output_sha256']:
            raise ValueError('Different reviewed original message output')
        target = args.output/(name+'.native.bin')
        command = [str(args.native), mode, str(bank), message]+(['Ari'] if mode=='pme-name' else [])+[str(target)]
        subprocess.run(command, check=True)
        if target.read_bytes() != original.read_bytes():
            raise ValueError('VIL selection/name output differs: '+name)
        report['cases'].append(dict(message=int(message), mode=mode, exact_bytes=target.stat().st_size,
                                    sha256=expected, original_run=run, command=command))
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print('Six VIL selection/name outputs exact; three music2 resources pinned')

if __name__ == '__main__':
    main()
