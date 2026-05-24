#!/usr/bin/env python3
"""Import the pointer-free rear-encounter banner window row from the owned ELF.

Field manager 0x133a98 phase 11 builds 0x11f5f8(manager, 0x2226f0) for a rear
("surprise attack") contact. Its single type-13 layer row is consumed by
0x11f7a0, which constructs and configures a UI window (0x198e38 capacities
from words 25/26, 0x198650 and setters from words 1..20, atlas list word 21).

OKRBN1\\0\\0 v1 is an 88-byte little-endian header followed by the 28-word row
(stream, atlas, constructor and updater words rewritten to logical ID 1), the
row's terminated atlas list (here only the all-zero terminator), and the
NUL-inclusive phase-11 argument string at 0x2284a0. The row's stream word
points at 0x1f86c0, whose file-backed bytes are zero; constructor 0x115b20
writes the stream at runtime (0x115ad8: opcode14/MenuTransform1f9c00, then
opcode6/MenuTransform1f9c2c). No guest pointer or stream state is serialized.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

from import_battle_data import ElfImage

ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
SOURCE_SHA256 = '9e42014fbf83588ce4728b58be4e08aaf42c81011ae7fd97536d5aeb47df71f9'
ROW_ADDRESS = 0x2226f0
ROW_SIZE = 112
ROW_TERMINATED_SIZE = ROW_SIZE + 4
STREAM_ADDRESS = 0x1f86c0
STREAM_BYTES = 20  # [14, ref, 6, ref, 0] once 0x115ad8 has run.
ATLAS_ADDRESS = 0x1f8610
ATLAS_TUPLE_SIZE = 16
ATLAS_MAX_TUPLES = 32
DISPATCH_TABLE_ADDRESS = 0x1f8f48
DISPATCH_SELECTOR = 13
DISPATCH_HANDLER = 0x115c48
ARGUMENT_ADDRESS = 0x2284a0
ARGUMENT = b'surprise attack\0'
MAGIC = b'OKRBN1\0\0'
VERSION = 1
HEADER = struct.Struct('<8sIIII32s32s')
ROW = struct.Struct('<28I')

SOURCE_BINDINGS = {
    0: STREAM_ADDRESS,
    21: ATLAS_ADDRESS,
    22: 0x115b20,
    23: 0x115bd8,
}
IMPORTED_BINDINGS = {index: 1 for index in SOURCE_BINDINGS}
EXPECTED_TYPE = 13
EXPECTED_CAPACITIES = (8, 2)

def extract_source(image):
    """Return source digest input, decoded row, terminated atlas and argument."""
    row_raw = image.read(ROW_ADDRESS, ROW_TERMINATED_SIZE)
    if row_raw[ROW_SIZE:] != bytes(4):
        raise ValueError('Rear banner row list terminator is not zero')
    words = ROW.unpack(row_raw[:ROW_SIZE])
    for index, address in SOURCE_BINDINGS.items():
        if words[index] != address:
            raise ValueError(f'Unsupported rear banner row binding at word {index}')
    if words[24] != EXPECTED_TYPE:
        raise ValueError(f'Unsupported rear banner row type: {words[24]}')
    if words[25:27] != EXPECTED_CAPACITIES:
        raise ValueError(f'Unsupported rear banner row capacities: {words[25:27]}')
    if words[20] != 1 or words[27] != 0:
        raise ValueError('Unsupported rear banner row layout flag or reserved word')

    stream = image.read(STREAM_ADDRESS, STREAM_BYTES)
    if stream != bytes(STREAM_BYTES):
        raise ValueError('Rear banner stream state is not the zero ELF image')

    atlas = []
    for index in range(ATLAS_MAX_TUPLES):
        raw = image.read(ATLAS_ADDRESS + index * ATLAS_TUPLE_SIZE, ATLAS_TUPLE_SIZE)
        tuple_words = struct.unpack('<4I', raw)
        atlas.append((raw, tuple_words))
        if tuple_words == (0, 0, 0, 0):
            break
    else:
        raise ValueError('Rear banner atlas has no all-zero tuple terminator within 32 entries')
    if len(atlas) != 1:
        raise ValueError('Unsupported rear banner atlas entries before terminator')

    dispatch = image.read(DISPATCH_TABLE_ADDRESS + DISPATCH_SELECTOR * 4, 4)
    if struct.unpack('<I', dispatch)[0] != DISPATCH_HANDLER:
        raise ValueError('Unsupported rear banner type-13 dispatch handler')

    argument = image.read(ARGUMENT_ADDRESS, len(ARGUMENT))
    if argument != ARGUMENT:
        raise ValueError('Unsupported rear banner phase-11 argument string')

    source = row_raw + stream + b''.join(raw for raw, _ in atlas) + dispatch + argument
    return source, words, atlas, argument

def import_elf(data):
    digest = hashlib.sha256(data).digest()
    if digest.hex() != ELF_SHA256:
        raise ValueError('Owned ELF SHA256 does not match supported retail revision')
    source, words, atlas, argument = extract_source(ElfImage(data))
    source_digest = hashlib.sha256(source).digest()
    if source_digest.hex() != SOURCE_SHA256:
        raise ValueError('Rear banner source literals SHA256 differ from supported literals')

    mapped = list(words)
    for index, binding_id in IMPORTED_BINDINGS.items():
        mapped[index] = binding_id
    payload = bytearray(ROW.pack(*mapped))
    payload.extend(b''.join(raw for raw, _ in atlas))
    payload.extend(argument)
    output = HEADER.pack(MAGIC, VERSION, 28, len(atlas), len(argument),
                         digest, source_digest) + payload
    metadata = {
        'schema': 'okage-rear-banner-import-v1',
        'scope': 'Pointer-free 0x2226f0 banner window row, its terminated atlas list and the 0x2284a0 argument; no callback behavior.',
        'elf_sha256': digest.hex(),
        'source_sha256': source_digest.hex(),
        'source_bytes': len(source),
        'source_ranges': [
            {'vaddr': f'0x{ROW_ADDRESS:08x}', 'bytes_including_terminator': ROW_TERMINATED_SIZE},
            {'vaddr': f'0x{STREAM_ADDRESS:08x}', 'bytes': STREAM_BYTES,
             'meaning': 'zero file-backed stream state; written at runtime by 0x115ad8'},
            {'vaddr': f'0x{ATLAS_ADDRESS:08x}', 'bytes': ATLAS_TUPLE_SIZE,
             'meaning': 'first all-zero 16-byte tuple terminates the atlas list'},
            {'vaddr': f'0x{DISPATCH_TABLE_ADDRESS + DISPATCH_SELECTOR * 4:08x}',
             'bytes': 4, 'meaning': 'type-13 dispatch target 0x00115c48'},
            {'vaddr': f'0x{ARGUMENT_ADDRESS:08x}', 'bytes_including_nul': len(argument),
             'bytes_hex': argument.hex()},
        ],
        'record': {
            'type': EXPECTED_TYPE,
            'capacities': list(EXPECTED_CAPACITIES),
            'bindings': {'stream_id': 1, 'atlas_id': 1,
                         'constructor_handler_id': 1, 'update_handler_id': 1},
        },
        'binding_meanings': {
            'stream_id_1': 'source stream address 0x001f86c0; written by 0x00115b20 -> 0x00115ad8',
            'atlas_id_1': 'terminated tuple list at 0x001f8610',
            'constructor_handler_id_1': 'source constructor 0x00115b20',
            'update_handler_id_1': 'source updater 0x00115bd8',
        },
        'notes': [
            'Only the first zero atlas tuple is imported; consumer 0x0011f7a0 stops at that tuple but still binds *0x24f730.',
            'Literal bytes are preserved without conversion.',
        ],
    }
    metadata['output_bytes'] = len(output)
    metadata['output_sha256'] = hashlib.sha256(output).hexdigest()
    return output, metadata

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
        with manifest.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(metadata, stream, indent=2, sort_keys=True)
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
        print(f'Rear banner import failed: {exc}', file=sys.stderr)
        return 2
    print(json.dumps({'bytes': len(data), 'output_sha256': metadata['output_sha256'],
                      'source_sha256': metadata['source_sha256']}))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
