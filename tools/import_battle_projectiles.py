#!/usr/bin/env python3
"""Import the owned battle projectile table referenced by action descriptors.

Source 0x154b38 selects ranged callback 0x1567c8 when descriptor word +4 is
nonzero; 0x156920/0x156ba8 read that pointer's record: signed16 type,
signed16 flight duration and three float32 launch offsets (16 bytes). The
pinned USA ELF holds 22 contiguous records at 0x237418..0x237578, ending at
the paired descriptor region. Every nonzero descriptor pointer must name one
of them; the artifact keeps the table base so native code can resolve the
retained descriptor word without any other guest address.

OKBPRJ1\\0: <8s5I32s32s> magic/version/base/count/stride/payload-size,
ELF/payload SHA256, then count*16 raw record bytes.
"""
import argparse
import hashlib
import json
import math
import struct
from pathlib import Path
from import_battle_data import ELF_SHA256, ElfImage
from import_battle_action_data import REGIONS

MAGIC = b'OKBPRJ1\0'
HEADER = struct.Struct('<8s5I32s32s')
BASE, COUNT, STRIDE = 0x237418, 22, 16

def sha256(data):
    return hashlib.sha256(data).hexdigest()

def extract(image):
    if BASE + COUNT*STRIDE != REGIONS[0][0]:
        raise ValueError('Projectile table must end at the paired descriptor region')
    table = image.read(BASE, COUNT*STRIDE)
    if len(table) != COUNT*STRIDE:
        raise ValueError('Truncated projectile table')
    references = {}
    for start, end, _, name in REGIONS:
        region = image.read(start, end-start)
        if len(region) != end-start:
            raise ValueError('Truncated descriptor region')
        for index in range((end-start)//0x88):
            pointer = struct.unpack_from('<I', region, index*0x88+4)[0]
            if not pointer:
                continue
            if pointer < BASE or pointer >= BASE+COUNT*STRIDE or (pointer-BASE) % STRIDE:
                raise ValueError(f'Descriptor projectile pointer outside table: {pointer:#x}')
            references.setdefault((pointer-BASE)//STRIDE, []).append({'region': name, 'index': index})
    records = []
    for index in range(COUNT):
        kind, duration, x, y, z = struct.unpack_from('<hh3f', table, index*STRIDE)
        if not all(math.isfinite(v) for v in (x, y, z)):
            raise ValueError('Nonfinite projectile offset')
        if duration <= 0:
            raise ValueError('Nonpositive projectile duration')
        records.append({'index': index, 'type': kind, 'duration': duration, 'offset': [x, y, z],
                        'references': references.get(index, [])})
    if set(references) != set(range(COUNT)):
        raise ValueError('Projectile table contains unreferenced records')
    return table, records

def import_data(elf):
    if sha256(elf) != ELF_SHA256:
        raise ValueError('Owned ELF SHA256 mismatch')
    table, records = extract(ElfImage(elf))
    artifact = HEADER.pack(MAGIC, 1, BASE, COUNT, STRIDE, len(table),
                           bytes.fromhex(ELF_SHA256), hashlib.sha256(table).digest()) + table
    metadata = {'schema': 'okage-battle-projectiles-v1', 'elf_sha256': ELF_SHA256,
                'base': hex(BASE), 'count': COUNT, 'stride': STRIDE, 'records': records,
                'payload_sha256': sha256(table), 'artifact_sha256': sha256(artifact),
                'source_helpers': ['0x154b38', '0x1567c8', '0x156920', '0x156ba8']}
    return artifact, metadata

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--manifest', type=Path)
    args = parser.parse_args(argv)
    manifest = args.manifest or args.output.with_name(args.output.name+'.manifest.json')
    if args.output.absolute() == manifest.absolute():
        raise ValueError('Output and manifest must differ')
    for path in (args.output, manifest):
        if path.exists() or path.is_symlink():
            raise ValueError(f'Refusing existing or symlink output: {path}')
    artifact, metadata = import_data(args.elf.read_bytes())
    for path in (args.output, manifest):
        path.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('xb') as stream:
        stream.write(artifact)
    with manifest.open('x') as stream:
        json.dump(metadata, stream, indent=2, sort_keys=True)
        stream.write('\n')
    return 0

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, struct.error) as error:
        raise SystemExit(f'import_battle_projectiles: {error}')
