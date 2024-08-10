#!/usr/bin/env python3
"""Import hash-pinned numbered RXWS sound sets from the locally prepared disc image."""
import argparse
import hashlib
import json
import os
from pathlib import Path
from inspect_disc import Disc
from verify_event import write_exact

HASHES = {
    1: '738c5cd9f3a77cc5bb576454ca928fb56554267858140aafb893334487b05f6c',
    2: 'e83e0e53c7cb372d96c33a1867b2e3b54c5d30711abddc999cd483eaade3470a',
    3: 'cadf8846ef74e1a6ab1fca6a819ebe04d76a4e143ad66b351ec96c378575bad8',
    4: 'b218222f870336244b685ab54f89e6e1e3c69411662b4d13539cc29399ce42b3',
    5: 'b218222f870336244b685ab54f89e6e1e3c69411662b4d13539cc29399ce42b3',
    6: 'b218222f870336244b685ab54f89e6e1e3c69411662b4d13539cc29399ce42b3',
    7: '3450875e4188817a0c48ca022f085adcaea0d26a793e66df9c02f907c43fe46e',
    8: 'd26a045aacf62dab22f8592357c687993b3d3029ff651cf7863acf231d30b450',
    9: '2a1fbcedc3ce7a3bd0ba212152fdcee1ed10b48d9669c2b4d26e29b4ceafe795',
    10: '84062be8c8b4d10d036e4c4ee4897a08cf875d57abe1bbd71748258ca887200a',
    11: '2e5c7bd2f1083c0213723505d505c50421b4146133fe94fd6daca7b5ccf1853f',
    12: 'e4c07efc18a723862d1bba4759d966c2f2da8d341270ec38b8edd100c20c820c',
    13: '467e05cbad272ab6428b8bdf2dab44b8754beb0b99504f24b6eb4efbf6da7011',
    14: 'f269d17f17bb1e6520455cb50df6bb3fa810d168ca1702956fab4608c1d0591b',
    15: '1018e38b81009c73f4febcce0542a173292a0ace17977420ddf50b9ca09de740',
    16: '2b7eb9599b9432f548d0ebe11e79755fe88b8aaa67b6a6cd923fa06e06b988cf',
    17: 'b70885de8e8c14e7e92e00254b8f4c2837a8fb49ac89c946f6db95488077f667',
    18: '6fe4e6bc02be9d908682fdb6b38d6aec037d4d34dcc16e4943761f9e65325be9',
    19: '5c2ab20db50361532fec612a5ba6d1c499ba07853e83e7b465f7e82e69ef1709',
    20: '6e6783b07aa96a0d7624abced2adf8f824a22e965ba54bf3e5e463e33ee9b479',
    21: '9869c3a9ab58e04c21440b39ee0b45fa0777873364b816b6889a771b1dc983e1',
    22: 'dcccf014032263053cf3881f94a7909ea0f113c7b5d18044fb8eeecbf2a89943',
    23: 'b4f7520fabc81bd56e6059496441c35ba857d00bfb9ecd6cee8f5055dd10de89',
    24: 'fd8c92ad3eb849735f681e28f1c078ec27faf8dcac2d29b057468b02bb176256',
    25: '1662c7d3ded1491c04cd523e02f1767dad21c9eebf149b5eb91b12415c09f9ac',
    26: 'c835f022373f591cc79995ae33bf0cc94a2d61312d7c55625b3673651cef181f',
    27: '478e1f93dfcc9ca16c306c8987748e4b9be273bfff5b65767b6358722297b149',
    28: 'bd69346e4c6862b4d0dddb4386265567036ab8c02329e5cd189c3f1b8f76797c',
    29: '2f8dc2c26fa2cefc53b3e43d4e716aba81c867a7e26230e0ce9be880af6d606',
    30: '63c44e02024f6e183740a7f685073d3b755d1e2a1692a4d8acd372c9cf61a09b',
    31: '792e23e2172edd738675737d234cb933b258a3af4e13833bad98fa714820d39f',
    32: 'f5c4d88f6d4cdae7bc8af2cf6ebc06ab5457cae9e3ee55a5a531768cba463e5a',
    33: '3c777eacd3f3f15e23bffc5db9cf26d4ab090c3f01ed85418ce9191e35d44fdc',
    34: 'a2e4ab71b7bfee951ed908800483ece359610f70f62f861ca747d2c6dd870123',
    35: 'bf2b01ad34b568ee8bb6dba6d80e60a98045e8f6f7bafff2b098f88670ee3ce0',
    36: '984335021846c3513cadc169fe81a133a6690c8158156690f896f075d80d2cf4',
}

REPORT_PATH = Path('docs/disc-report.json')
DISC_PATH = Path('local/disc/Okage - Shadow King (USA).bin')
ASSET_ROOT = Path('local/audio-input')
INVENTORY_SCHEMA = 'rxws-numbered-set-inventory-v1'

def read_pinned_sets(sound_sets, entries_path=REPORT_PATH, disc_path=DISC_PATH):
    """Read and verify all selected extents before any destination is written."""
    entries = json.loads(Path(entries_path).read_text())['files']
    by_path = {entry['path']: entry for entry in entries}
    selected = []
    for sound_set in sound_sets:
        name = f'SNDDATA/SE/SET{sound_set}.XWS'
        try:
            entry = by_path[name]
        except KeyError as exc:
            raise ValueError(f'Missing owned disc entry: {name}') from exc
        selected.append((sound_set, name, entry))

    disc = Disc(disc_path)
    try:
        verified = []
        for sound_set, name, entry in selected:
            data = disc.read(entry['lba'], entry['size'])
            sha = hashlib.sha256(data).hexdigest()
            if sha != HASHES[sound_set]:
                raise ValueError(f'Different owned sound-set resource: {name}')
            verified.append((sound_set, name, entry, data, sha))
        return verified
    finally:
        disc.close()

def manifest_item(name, entry, data, sha, destination):
    return dict(path=name, lba=entry['lba'], size=len(data), sha256=sha,
                destination=str(destination))

def preflight(outputs):
    """Reject conflicts and symlink paths before the first output is written."""
    normalized = []
    for path, payload in outputs:
        path = Path(path)
        raw_absolute = path if path.is_absolute() else Path.cwd() / path
        current = Path(raw_absolute.anchor)
        for part in raw_absolute.parts[1:]:
            if part == '..':
                current = current.parent
            elif part != '.':
                current /= part
                if current.is_symlink():
                    raise RuntimeError('Refusing symlink output path component: ' + str(current))

        absolute = Path(os.path.abspath(path))
        normalized.append((absolute, path, payload))

    for index, (absolute, path, _) in enumerate(normalized):
        for other, _, _ in normalized[index + 1:]:
            if absolute == other:
                raise RuntimeError('Refusing duplicate output path: ' + str(path))
            if absolute in other.parents or other in absolute.parents:
                raise RuntimeError('Refusing output path ancestor conflict: ' + str(path))

    for absolute, path, payload in normalized:
        if absolute.exists() and absolute.read_bytes() != payload:
            raise RuntimeError('Refusing changed output: ' + str(path))

def import_sets(sound_sets, output, all_sets=False, entries_path=REPORT_PATH,
                disc_path=DISC_PATH, asset_root=ASSET_ROOT):
    verified = read_pinned_sets(sound_sets, entries_path, disc_path)
    records, outputs = [], []
    for sound_set, name, entry, data, sha in verified:
        destination = Path(asset_root) / name
        outputs.append((destination, data))
        records.append(manifest_item(name, entry, data, sha, destination))
    if all_sets:
        manifest = {'schema': INVENTORY_SCHEMA, 'sets': records}
    else:
        manifest = records[0]
    manifest_data = (json.dumps(manifest, indent=2) + '\n').encode()
    outputs.append((Path(output), manifest_data))

    # Check all bytes and names before making directories or writing anything.
    preflight(outputs)
    for path, _ in outputs:
        path.parent.mkdir(parents=True, exist_ok=True)
    for path, payload in outputs:
        write_exact(path, payload)
    return records

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument('--set', type=int, choices=HASHES,
                           help='Import one numbered set')
    selection.add_argument('--all', action='store_true',
                           help='Import all owned numbered sets SET1 through SET36')
    parser.add_argument('--output', type=Path, required=True,
                        help='Import manifest; existing changed outputs are refused')
    args = parser.parse_args(argv)
    sound_sets = range(1, 37) if args.all else (args.set,)
    records = import_sets(sound_sets, args.output, all_sets=args.all)
    for record in records:
        print(record['path'], record['sha256'])

if __name__ == '__main__':
    main()
