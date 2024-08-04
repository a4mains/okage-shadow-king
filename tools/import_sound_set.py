#!/usr/bin/env python3
"""Import a reviewed RXWS sound set from the locally prepared owned disc image."""
import argparse
import hashlib
import json
from pathlib import Path
from inspect_disc import Disc
from verify_event import write_exact

HASHES = {
    1: '738c5cd9f3a77cc5bb576454ca928fb56554267858140aafb893334487b05f6c',
    2: 'e83e0e53c7cb372d96c33a1867b2e3b54c5d30711abddc999cd483eaade3470a',
    10: '84062be8c8b4d10d036e4c4ee4897a08cf875d57abe1bbd71748258ca887200a',
    11: '2e5c7bd2f1083c0213723505d505c50421b4146133fe94fd6daca7b5ccf1853f',
    12: 'e4c07efc18a723862d1bba4759d966c2f2da8d341270ec38b8edd100c20c820c',
    13: '467e05cbad272ab6428b8bdf2dab44b8754beb0b99504f24b6eb4efbf6da7011',
    14: 'f269d17f17bb1e6520455cb50df6bb3fa810d168ca1702956fab4608c1d0591b',
    15: '1018e38b81009c73f4febcce0542a173292a0ace17977420ddf50b9ca09de740',
    16: '2b7eb9599b9432f548d0ebe11e79755fe88b8aaa67b6a6cd923fa06e06b988cf',
    17: 'b70885de8e8c14e7e92e00254b8f4c2837a8fb49ac89c946f6db95488077f667',
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--set', type=int, choices=HASHES, required=True, dest='sound_set')
    parser.add_argument('--output', type=Path, required=True, help='Import manifest; existing changed outputs are refused')
    args = parser.parse_args()
    name = f'SNDDATA/SE/SET{args.sound_set}.XWS'
    entries = json.loads(Path('docs/disc-report.json').read_text())['files']
    entry = next(e for e in entries if e['path'] == name)
    disc = Disc('local/disc/Okage - Shadow King (USA).bin')
    try:
        data = disc.read(entry['lba'], entry['size'])
    finally:
        disc.close()
    sha = hashlib.sha256(data).hexdigest()
    if sha != HASHES[args.sound_set]:
        raise ValueError('Different owned sound-set resource')
    destination = Path('local/audio-input')/name
    destination.parent.mkdir(parents=True, exist_ok=True)
    write_exact(destination, data)
    manifest = dict(path=name, lba=entry['lba'], size=len(data), sha256=sha, destination=str(destination))
    write_exact(args.output, (json.dumps(manifest, indent=2)+'\n').encode())
    print(name, sha)

if __name__ == '__main__':
    main()
