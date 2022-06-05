#!/usr/bin/env python3
"""Import the two authored battle UI interface records as pointer-free data.

OKBTUI1\0 v1 uses an 88-byte little-endian header
  <8sIIII32s32s: magic, version, records, words/record, record bytes,
  owned ELF SHA256, source table SHA256.
It is followed by two 112-byte rows of 28 u32 words. Texture binding is shared
by the native interface and is not serialized as an authored field. Rows retain source words
1..20 and 24..27 verbatim. Pointer/code words are replaced: word 0 is a
controller-stream binding (1=type-2 stream, 2=type-3 stream), word 21 is the
shared runtime atlas-table binding (1), word 22 is the constructor handler
(1=type 2, 2=type 3), and word 23 is the callback handler (1=type 2, 2=type
3). Header texture binding 1 means the host-owned loaded battle UI texture
source (original global 0x24f730). No guest address is emitted.

The source digest covers exactly 0xe4 bytes at ELF virtual address 0x248100:
two 112-byte records and the four-byte zero terminator at 0x2481e0.
"Atlas table" remains a runtime host binding: static zero bytes at 0x1fdba8
do not prove that the mutable runtime source table is empty.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

from import_battle_data import ElfImage

ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
TABLE_ADDRESS = 0x248100
RECORD_COUNT = 2
RECORD_SIZE = 112
TERMINATED_TABLE_SIZE = RECORD_COUNT * RECORD_SIZE + 4
MAGIC = b'OKBTUI1\0'
VERSION = 1
TEXTURE_BINDING_ID = 1
HEADER = struct.Struct('<8sIIII32s32s')
RECORD = struct.Struct('<28I')

# Source guest pointers/code addresses are checked here and replaced by IDs.
EXPECTED = (
    {'stream': 0x1fd820, 'atlas': 0x1fdba8, 'ctor': 0x163fb8,
     'callback': 0x164090, 'type': 2, 'capacity_a': 29, 'capacity_b': 17},
    {'stream': 0x1fdb88, 'atlas': 0x1fdba8, 'ctor': 0x168228,
     'callback': 0x1684d0, 'type': 3, 'capacity_a': 13, 'capacity_b': 8},
)

def extract_table(image):
    """Return the validated authored table bytes; image must map ELF VAs."""
    raw = image.read(TABLE_ADDRESS, TERMINATED_TABLE_SIZE)
    if raw[RECORD_COUNT * RECORD_SIZE:] != bytes(4):
        raise ValueError('Interface table terminator at 0x2481e0 is not zero')
    records = []
    for index, expected in enumerate(EXPECTED):
        record = raw[index * RECORD_SIZE:(index + 1) * RECORD_SIZE]
        words = struct.unpack('<28I', record)
        checks = ((0, 'stream'), (21, 'atlas'), (22, 'ctor'), (23, 'callback'),
                  (24, 'type'), (25, 'capacity_a'), (26, 'capacity_b'))
        for word_index, label in checks:
            if words[word_index] != expected[label]:
                raise ValueError(f'Unsupported interface row {index} {label}: {words[word_index]:#x}')
        if words[27] != 0:
            raise ValueError(f'Unsupported interface row {index} reserved word')
        if words[1:4] != (40, 6, 2):
            raise ValueError(f'Unsupported interface row {index} window arguments')
        records.append(words)
    return raw, records

def import_elf(data):
    digest = hashlib.sha256(data).digest()
    if digest.hex() != ELF_SHA256:
        raise ValueError('Owned ELF SHA256 does not match supported retail revision')
    raw, records = extract_table(ElfImage(data))
    rows = []
    for index, words in enumerate(records):
        mapped = list(words)
        mapped[0] = index + 1                       # controller stream binding
        mapped[21] = 1                               # runtime atlas table binding
        mapped[22] = index + 1                       # constructor handler ID
        mapped[23] = index + 1                       # update callback handler ID
        rows.extend(mapped)
    table_digest = hashlib.sha256(raw).digest()
    header = HEADER.pack(MAGIC, VERSION, RECORD_COUNT, 28, RECORD_SIZE,
                         digest, table_digest)
    return header + struct.pack('<' + 'I' * len(rows), *rows), {
        'schema': 'okage-battle-ui-import-v1',
        'elf_sha256': digest.hex(),
        'table_sha256': table_digest.hex(),
        'table_vaddr': f'0x{TABLE_ADDRESS:08x}',
        'table_bytes_including_terminator': len(raw),
        'output_sha256': hashlib.sha256(header + struct.pack('<' + 'I' * len(rows), *rows)).hexdigest(),
        'records': [
            {'index': 0, 'type': 2, 'stream_binding_id': 1, 'atlas_binding_id': 1,
             'texture_binding_id': TEXTURE_BINDING_ID, 'constructor_handler_id': 1,
             'update_handler_id': 1, 'capacities': [29, 17]},
            {'index': 1, 'type': 3, 'stream_binding_id': 2, 'atlas_binding_id': 1,
             'texture_binding_id': TEXTURE_BINDING_ID, 'constructor_handler_id': 2,
             'update_handler_id': 2, 'capacities': [13, 8]},
        ],
        'bindings': {
            'stream_binding_id': 'per-record owned controller stream slot',
            'atlas_binding_id_1': 'runtime-populated atlas region table; entries intentionally not imported',
            'texture_binding_id_1': 'host binding for loaded UI texture formerly referenced by global 0x24f730',
            'constructor_handler_id_1': 'type-2 constructor formerly at 0x163fb8',
            'constructor_handler_id_2': 'type-3 constructor formerly at 0x168228',
            'update_handler_id_1': 'type-2 callback formerly at 0x164090',
            'update_handler_id_2': 'type-3 callback formerly at 0x1684d0',
        },
    }

def write_outputs(output, manifest, data, metadata):
    output, manifest = Path(output), Path(manifest)
    for path in (output, manifest):
        if path.is_symlink():
            raise ValueError(f'Refusing symlink output: {path}')
        if path.exists():
            raise ValueError(f'Refusing existing output: {path}')
    output.parent.mkdir(parents=True, exist_ok=True)
    manifest.parent.mkdir(parents=True, exist_ok=True)
    written = []
    try:
        with output.open('xb') as stream:
            stream.write(data)
        written.append(output)
        with manifest.open('x', encoding='utf-8') as stream:
            json.dump(metadata, stream, indent=2)
            stream.write('\n')
        written.append(manifest)
    except OSError:
        for path in written:
            path.unlink(missing_ok=True)
        raise

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--manifest', type=Path)
    args = parser.parse_args(argv)
    manifest = args.manifest or args.output.with_name(args.output.name + '.manifest.json')
    try:
        data, metadata = import_elf(args.elf.read_bytes())
        write_outputs(args.output, manifest, data, metadata)
    except (OSError, ValueError, struct.error) as exc:
        print(f'battle UI import failed: {exc}', file=sys.stderr)
        return 2
    print(json.dumps({'bytes': len(data), 'output_sha256': metadata['output_sha256'],
                      'table_sha256': metadata['table_sha256']}))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
