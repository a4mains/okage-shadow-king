#!/usr/bin/env python3
"""Import pointer-free New Game name-entry interface literals from the owned ELF.

OKNMED2\0 v2 is an 88-byte little-endian header followed by:
  * one 112-byte, 28-word interface row;
  * the first 16-byte all-zero atlas tuple, which terminates the authored list;
  * four raw 8-byte scalar literals (the source binary's double constants);
  * the four default-name bytes at 0x222aa8;
  * two NUL-inclusive, length-prefixed strings.

The header is <8sIIII32s32s: magic, version, row count, words/row, row bytes,
owned ELF SHA256, SHA256 of all source bytes in address-list order. The row's
stream, atlas, constructor, and callback guest addresses are validated and
rewritten as logical IDs 1; all other authored row words are preserved.
The atlas terminator is proven by the consumer at 0x11f7a0, which reads up to
32 16-byte tuples and stops at the first all-zero tuple. The ELF's first tuple
at 0x1f88c8 is all zero, so no subsequent adjacent data belongs in this table.

This remains data import only. It does not implement keypad/controller logic.
The system-message lookups used by 0x123530 and 0x1256a8 use the existing
GameDataTables::system_message provider rather than a duplicate text table.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

from import_battle_data import ElfImage

ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
SOURCE_SHA256 = 'b8356ed62e594a4c2b1e9dc550df48200987ddd861fd8862057fa1cb3820be74'
ROW_ADDRESS = 0x222ae0
ROW_SIZE = 112
TERMINATED_ROW_SIZE = ROW_SIZE + 4
ATLAS_ADDRESS = 0x1f88c8
ATLAS_TUPLE_SIZE = 16
ATLAS_MAX_TUPLES = 32
DOUBLE_ADDRESSES = (0x222a88, 0x222a90, 0x222a98, 0x222aa0)
DEFAULT_NAME_ADDRESS = 0x222aa8
DEFAULT_NAME_SIZE = 4
STRING_ADDRESSES = (0x222a80, 0x222ab0)
STRING_MAX_SIZE = 64
MAGIC = b'OKNMED2\0'
VERSION = 2
HEADER = struct.Struct('<8sIIII32s32s')
ROW = struct.Struct('<28I')
STRING_LENGTH = struct.Struct('<I')

SOURCE_BINDINGS = {
    0: 0x1f8978,  # name-entry stream
    21: 0x1f88c8, # shared runtime atlas tuple table
    22: 0x1196a0, # constructor
    23: 0x119700, # update callback
}
IMPORTED_BINDINGS = {0: 1, 21: 1, 22: 1, 23: 1}
EXPECTED_TYPE = 9
EXPECTED_CAPACITIES = (32, 16)

def bounded_string(image, address):
    """Read a short NUL-terminated source byte string without decoding it."""
    result = bytearray()
    for offset in range(STRING_MAX_SIZE):
        value = image.read(address + offset, 1)
        result.extend(value)
        if value == b'\0':
            return bytes(result)
    raise ValueError(f'Unterminated name-entry string at {address:#x}')

def extract_source(image):
    """Return exact validated source bytes and decoded fixed-shape fields."""
    row_raw = image.read(ROW_ADDRESS, TERMINATED_ROW_SIZE)
    if row_raw[ROW_SIZE:] != bytes(4):
        raise ValueError('Name-entry interface terminator is not zero')
    words = ROW.unpack(row_raw[:ROW_SIZE])
    for index, address in SOURCE_BINDINGS.items():
        if words[index] != address:
            raise ValueError(f'Unsupported name-entry binding at word {index}')
    if words[24] != EXPECTED_TYPE:
        raise ValueError(f'Unsupported name-entry type: {words[24]}')
    if words[25:27] != EXPECTED_CAPACITIES:
        raise ValueError(f'Unsupported name-entry capacities: {words[25:27]}')
    if words[27] != 0:
        raise ValueError('Unsupported name-entry reserved word')

    atlas_terminator = image.read(ATLAS_ADDRESS, ATLAS_TUPLE_SIZE)
    if atlas_terminator != bytes(ATLAS_TUPLE_SIZE):
        raise ValueError('Name-entry atlas first tuple is not the source terminator')
    doubles = b''.join(image.read(address, 8) for address in DOUBLE_ADDRESSES)
    default_name = image.read(DEFAULT_NAME_ADDRESS, DEFAULT_NAME_SIZE)
    strings = tuple(bounded_string(image, address) for address in STRING_ADDRESSES)
    if any(len(value) > STRING_MAX_SIZE for value in strings):
        raise ValueError('Name-entry string exceeds the bounded source extent')

    source = row_raw + atlas_terminator + doubles + default_name + b''.join(strings)
    return source, words, atlas_terminator, doubles, default_name, strings

def import_elf(data):
    digest = hashlib.sha256(data).digest()
    if digest.hex() != ELF_SHA256:
        raise ValueError('Owned ELF SHA256 does not match supported retail revision')
    source, words, atlas, doubles, default_name, strings = extract_source(ElfImage(data))
    source_digest = hashlib.sha256(source).digest()
    if source_digest.hex() != SOURCE_SHA256:
        raise ValueError('Name-entry source literals SHA256 differ from supported literals')

    mapped = list(words)
    for index, binding_id in IMPORTED_BINDINGS.items():
        mapped[index] = binding_id
    payload = bytearray(ROW.pack(*mapped))
    payload.extend(atlas)
    payload.extend(doubles)
    payload.extend(default_name)
    for value in strings:
        payload.extend(STRING_LENGTH.pack(len(value)))
        payload.extend(value)
    output = HEADER.pack(MAGIC, VERSION, 1, 28, ROW_SIZE, digest, source_digest) + payload
    metadata = {
        'schema': 'okage-name-entry-import-v2',
        'elf_sha256': digest.hex(),
        'source_sha256': source_digest.hex(),
        'source_bytes': len(source),
        'source_ranges': [
            {'vaddr': f'0x{ROW_ADDRESS:08x}', 'bytes_including_terminator': TERMINATED_ROW_SIZE},
            {'vaddr': f'0x{ATLAS_ADDRESS:08x}', 'bytes': ATLAS_TUPLE_SIZE,
             'meaning': 'first all-zero tuple terminates the source atlas list'},
            *[{'vaddr': f'0x{address:08x}', 'bytes': 8, 'meaning': 'raw double literal'}
              for address in DOUBLE_ADDRESSES],
            {'vaddr': f'0x{DEFAULT_NAME_ADDRESS:08x}', 'bytes': DEFAULT_NAME_SIZE,
             'meaning': 'default name bytes, NUL included'},
            *[{'vaddr': f'0x{address:08x}', 'bytes': len(value),
               'meaning': 'bounded NUL-inclusive encoded string', 'bytes_hex': value.hex()}
              for address, value in zip(STRING_ADDRESSES, strings)],
        ],
        'output_bytes': len(output),
        'output_sha256': hashlib.sha256(output).hexdigest(),
        'record': {
            'type': EXPECTED_TYPE,
            'capacities': list(EXPECTED_CAPACITIES),
            'bindings': {'stream_id': 1, 'atlas_id': 1,
                         'constructor_handler_id': 1, 'update_handler_id': 1},
        },
        'binding_meanings': {
            'stream_id_1': 'name-entry stream formerly at 0x1f8978',
            'atlas_id_1': 'terminating source atlas table formerly at 0x1f88c8',
            'constructor_handler_id_1': 'constructor formerly at 0x1196a0',
            'update_handler_id_1': 'update callback formerly at 0x119700',
        },
        'atlas': {'tuple_size': ATLAS_TUPLE_SIZE, 'maximum_tuples': ATLAS_MAX_TUPLES,
                  'imported_tuples_including_terminator': 1, 'entries_before_terminator': 0},
        'strings': [{'source_vaddr': f'0x{address:08x}', 'bytes_hex': value.hex()}
                    for address, value in zip(STRING_ADDRESSES, strings)],
        'system_message_provider': 'GameDataTables::system_message for the source message lookups; no duplicate text table imported',
        'scope': 'Pointer-free interface/static literals only; keypad update semantics and controller behavior are not implemented.',
    }
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
        print(f'name-entry import failed: {exc}', file=sys.stderr)
        return 2
    print(json.dumps({'bytes': len(data), 'output_sha256': metadata['output_sha256'],
                      'source_sha256': metadata['source_sha256']}))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
