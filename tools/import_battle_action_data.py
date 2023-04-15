#!/usr/bin/env python3
"""Import static battle action descriptors from owned ELF and BKDATA.

OKBACT1\0: <8s8I32s32s32s>, version/unit-map-count/action-map-count/pair-count/
generic-count/secondary-count/record-size/payload-size, ELF/BKDATA/payload SHA256.
Payload: two signed16 mapping tables; paired, generic and secondary 0x88 records.
No guest addresses enter the artifact. Source helpers158258/157ed0 derive selection.
The unit map ends at independently referenced23df98 (256entries), paired records
span237578..23dd98 (98pairs), generic records23df98..243410 (159), secondary records
243410..243c08 (15). The action map imports256 entries corresponding to the whole
owned waza table, not a claimed unchecked-helper bound or inferred larger array.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from import_battle_data import ELF_SHA256, ElfImage

BKDATA_SHA256 = '973964019ddb24d033b0e10f4f3e2cf72fafdee9bdb25ee143ee87c832b0aff4'
MAGIC = b'OKBACT1\0'
HEADER = struct.Struct('<8s8I32s32s32s')
RECORD_BYTES = 0x88
REGIONS = ((0x237578, 0x23dd98, 0x110, 'paired'),
           (0x23df98, 0x243410, 0x88, 'generic'),
           (0x243410, 0x243c08, 0x88, 'secondary'))

def sha256(data):
    return hashlib.sha256(data).hexdigest()

def waza_count(data):
    def word(at):
        if at < 0 or at+4 > len(data):
            raise ValueError('Truncated XDB')
        return struct.unpack_from('<I', data, at)[0]
    if word(0) != 0x424458 or word(16) != 0xffffffff:
        raise ValueError('Unsupported BKDATA XDB')
    sections = word(12)
    base = 32 + sections*16
    strings = base + word(4)
    if sections > 65536 or strings > len(data) or strings + word(8) != len(data):
        raise ValueError('XDB extent')
    def text(offset):
        if offset >= word(8):
            raise ValueError('XDB string extent')
        end = data.find(b'\0', strings+offset)
        if end < 0:
            raise ValueError('XDB string terminator')
        return data[strings+offset:end]
    matches = []
    for i in range(sections):
        at = 32+i*16
        if text(word(at)) != b'waza':
            continue
        offset, words = word(at+4), word(at+8)
        if offset % 4 or words == 0 or words % 13 or base+offset+words*4 > strings:
            raise ValueError('Invalid waza extent')
        for row in range(words//13):
            for col in (0, 12):
                text(word(base+offset+(row*13+col)*4))
        matches.append(words//13)
    if len(matches) != 1:
        raise ValueError('Missing or duplicate waza table')
    return matches[0]

def extract_payload(image, actions):
    if actions != 256:
        raise ValueError('Unsupported owned waza count')
    unit_map = image.read(0x23dd98, 0x23df98-0x23dd98)
    action_map = image.read(0x243c08, actions*2)
    if len(unit_map) != 512 or len(action_map) != actions*2:
        raise ValueError('Truncated descriptor mapping')
    unit_ids = struct.unpack('<256h', unit_map)
    action_ids = struct.unpack('<256h', action_map)
    if any(x < 0 or x >= 98 for x in unit_ids) or any(x < 0 or x >= 159 for x in action_ids):
        raise ValueError('Descriptor mapping outside independent region')
    data = bytearray(unit_map+action_map)
    regions = []
    for start, end, stride, name in REGIONS:
        if (end-start) % stride:
            raise ValueError('Descriptor region alignment')
        raw = image.read(start, end-start)
        if len(raw) != end-start:
            raise ValueError('Truncated descriptor region')
        data.extend(raw)
        regions.append({'name': name, 'start': hex(start), 'end_exclusive': hex(end),
                        'stride': stride, 'count': (end-start)//stride, 'sha256': sha256(raw)})
    return bytes(data), {'regions': regions, 'unit_mapping_sha256': sha256(unit_map),
                         'action_mapping_sha256': sha256(action_map),
                         'action_mapping_scope': 'all256 owned waza rows; helper itself unchecked'}

def import_data(elf, bkdata):
    if sha256(elf) != ELF_SHA256 or sha256(bkdata) != BKDATA_SHA256:
        raise ValueError('Owned ELF/BKDATA SHA256 mismatch')
    count = waza_count(bkdata)
    payload, metadata = extract_payload(ElfImage(elf), count)
    artifact = HEADER.pack(MAGIC, 1, 256, count, 98, 159, 15, RECORD_BYTES, len(payload),
                           bytes.fromhex(ELF_SHA256), bytes.fromhex(BKDATA_SHA256),
                           hashlib.sha256(payload).digest()) + payload
    metadata.update(schema='okage-battle-action-data-v1', elf_sha256=ELF_SHA256,
                    bkdata_sha256=BKDATA_SHA256, waza_count=count,
                    payload_sha256=sha256(payload), artifact_sha256=sha256(artifact),
                    artifact_bytes=len(artifact), source_helpers=['0x158258', '0x157ed0', '0x1254a0'])
    return artifact, metadata

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--bkdata', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--manifest', type=Path)
    args = parser.parse_args(argv)
    manifest = args.manifest or args.output.with_name(args.output.name+'.manifest.json')
    if args.output.absolute() == manifest.absolute():
        raise ValueError('Output and manifest must differ')
    for path in (args.output, manifest):
        if path.exists() or path.is_symlink():
            raise ValueError(f'Refusing existing or symlink output: {path}')
    artifact, metadata = import_data(args.elf.read_bytes(), args.bkdata.read_bytes())
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
        raise SystemExit(f'import_battle_action_data: {error}')
