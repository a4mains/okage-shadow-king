#!/usr/bin/env python3
"""Import pointer-free Restore UI literals from the pinned owned ELF.

OKRST1\0\0 v1 is an 88-byte little-endian header followed by the 28-word
Restore row, its authored all-zero 16-byte atlas terminator, then fixed-ID
NUL-inclusive strings. The row's stream, atlas, constructor, and updater words
are rewritten to logical ID 1. String IDs are named in the C++ companion API;
the metadata retains their source addresses and raw bytes.

The static Restore row points at 0x24f6d0 for its stream state, but that address
has no file-backed bytes in this ELF. Source constructor 0x11b518 initializes
that stream at runtime as opcode14/MenuTransform1f9c00, opcode6/
MenuTransform1f9c2c, then the list terminator. This importer records no guest
pointer or invented stream state; the native API exposes the two proven
commands separately.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

from import_battle_data import ElfImage

ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
SOURCE_SHA256 = '35df7df98ca463c9e9a7e5e326bd56d4384b3cb48d770b1c08b849ad0c5a4f1f'
ROW_ADDRESS = 0x2233f0
ROW_SIZE = 112
ROW_TERMINATED_SIZE = ROW_SIZE + 4
ATLAS_ADDRESS = 0x1f8e98
ATLAS_TUPLE_SIZE = 16
ATLAS_MAX_TUPLES = 32
DISPATCH_TABLE_ADDRESS = 0x1f8f48
DISPATCH_SELECTOR = 8
DISPATCH_HANDLER = 0x11e480
STRING_MAX_SIZE = 256
MAGIC = b'OKRST1\0\0'
VERSION = 1
HEADER = struct.Struct('<8sIIII32s32s')
ROW = struct.Struct('<28I')
STRING_HEADER = struct.Struct('<II')

# (numeric ID, stable enum name, pinned ELF address).
STRING_SPECS = (
    (1, 'Slash', 0x222c00),
    (2, 'ProductCode', 0x222c08),
    (3, 'TextPrefix', 0x222c48),
    (4, 'FormatYesNo', 0x222c50),
    (5, 'YesNo', 0x222c68),
    (6, 'SaveSlotPrompt', 0x222c70),
    (7, 'ReadSlotPrompt', 0x222c90),
    (8, 'SlotLabels', 0x222cb8),
    (9, 'Empty', 0x222cd8),
    (10, 'NameFormat', 0x222ce0),
    (11, 'PlayerNameFormat', 0x222cf8),
    (12, 'PlayTimeFormat', 0x222d08),
    (13, 'HitPointsFormat', 0x222d18),
    (14, 'LevelFormat', 0x222d28),
    (15, 'NeutralStatus', 0x222d30),
    (16, 'GenericStringFormat', 0x222d48),
    (17, 'CurseStatusFormat', 0x222d50),
    (18, 'SlotNumberFormat', 0x222d70),
    (19, 'MenuNumberFormat', 0x222d78),
    (20, 'ProgressNumberFormat', 0x222d80),
    (21, 'CheckingCardNotice', 0x222d90),
    (22, 'NoCardNotice', 0x222dd8),
    (23, 'UnformattedCardNotice', 0x222e00),
    (24, 'CancelSaveNotice', 0x222e30),
    (25, 'FormattingNotice', 0x222e58),
    (26, 'FormatFailedNotice', 0x222e90),
    (27, 'SaveFilePathFormat', 0x222ea8),
    (28, 'NewSaveSpaceNotice', 0x222ed0),
    (29, 'IconSysPathFormat', 0x222f30),
    (30, 'SaveIcon1PathFormat', 0x222f40),
    (31, 'SaveIcon2PathFormat', 0x222f50),
    (32, 'SaveIcon3PathFormat', 0x222f60),
    (33, 'PathJoinFormat', 0x222f70),
    (34, 'CorruptSaveNotice', 0x222f78),
    (35, 'ReplaceSavePrompt', 0x222fc0),
    (36, 'ExistingSaveSpaceNotice', 0x222fe8),
    (37, 'SavingNotice', 0x223048),
    (38, 'DataDirectory', 0x223080),
    (39, 'SaveCompleteNotice', 0x223090),
    (40, 'SaveFailedNotice', 0x2230a8),
    (41, 'NoSavedDataNotice', 0x2230c0),
    (42, 'ReadFilePrompt', 0x223130),
    (43, 'LoadSuccessNotice', 0x223148),
    (44, 'LoadFailedNotice', 0x223168),
)

SOURCE_BINDINGS = {
    0: 0x24f6d0,  # initialized as a guest-owned stream by 0x11b518.
    21: ATLAS_ADDRESS,
    22: 0x11b518,
    23: 0x11c2c8,
}
IMPORTED_BINDINGS = {index: 1 for index in SOURCE_BINDINGS}
EXPECTED_TYPE = 8
EXPECTED_CAPACITIES = (17, 16)

def _read_c_string(image, address):
    value = bytearray()
    for offset in range(STRING_MAX_SIZE):
        byte = image.read(address + offset, 1)
        if byte == b'\0':
            return bytes(value) + b'\0'
        value.extend(byte)
    raise ValueError(f'Unterminated Restore string at {address:#x}')

def extract_source(image):
    """Return source digest input, decoded row, terminated atlas, and strings."""
    row_raw = image.read(ROW_ADDRESS, ROW_TERMINATED_SIZE)
    if row_raw[ROW_SIZE:] != bytes(4):
        raise ValueError('Restore interface row terminator is not zero')
    words = ROW.unpack(row_raw[:ROW_SIZE])
    for index, address in SOURCE_BINDINGS.items():
        if words[index] != address:
            raise ValueError(f'Unsupported Restore row binding at word {index}')
    if words[24] != EXPECTED_TYPE:
        raise ValueError(f'Unsupported Restore row type: {words[24]}')
    if words[25:27] != EXPECTED_CAPACITIES:
        raise ValueError(f'Unsupported Restore row capacities: {words[25:27]}')
    if words[27] != 0:
        raise ValueError('Unsupported Restore row reserved word')

    atlas = []
    for index in range(ATLAS_MAX_TUPLES):
        raw = image.read(ATLAS_ADDRESS + index * ATLAS_TUPLE_SIZE, ATLAS_TUPLE_SIZE)
        tuple_words = struct.unpack('<4I', raw)
        atlas.append((raw, tuple_words))
        if tuple_words == (0, 0, 0, 0):
            break
    else:
        raise ValueError('Restore atlas has no all-zero tuple terminator within 32 entries')
    if len(atlas) != 1:
        raise ValueError('Unsupported Restore atlas entries before terminator')

    dispatch = image.read(DISPATCH_TABLE_ADDRESS + DISPATCH_SELECTOR * 4, 4)
    if struct.unpack('<I', dispatch)[0] != DISPATCH_HANDLER:
        raise ValueError('Unsupported Restore type-8 dispatch handler')

    strings = []
    for string_id, key, address in STRING_SPECS:
        value = _read_c_string(image, address)
        strings.append((string_id, key, address, value))

    source = row_raw + b''.join(raw for raw, _ in atlas) + dispatch + b''.join(
        value for _, _, _, value in strings)
    return source, words, atlas, strings, dispatch

def import_elf(data):
    digest = hashlib.sha256(data).digest()
    if digest.hex() != ELF_SHA256:
        raise ValueError('Owned ELF SHA256 does not match supported retail revision')
    source, words, atlas, strings, dispatch = extract_source(ElfImage(data))
    source_digest = hashlib.sha256(source).digest()
    if source_digest.hex() != SOURCE_SHA256:
        raise ValueError('Restore source literals SHA256 differ from supported literals')

    mapped = list(words)
    for index, binding_id in IMPORTED_BINDINGS.items():
        mapped[index] = binding_id
    payload = bytearray(ROW.pack(*mapped))
    payload.extend(b''.join(raw for raw, _ in atlas))
    for string_id, _, _, value in strings:
        payload.extend(STRING_HEADER.pack(string_id, len(value)))
        payload.extend(value)
    output = HEADER.pack(MAGIC, VERSION, 28, len(atlas), len(strings),
                         digest, source_digest) + payload
    metadata = {
        'schema': 'okage-restore-data-import-v1',
        'scope': 'Pointer-free Restore descriptor and referenced static literal bytes; no native save behavior or callback implementation.',
        'elf_sha256': digest.hex(),
        'source_sha256': source_digest.hex(),
        'source_bytes': len(source),
        'source_ranges': [
            {'vaddr': f'0x{ROW_ADDRESS:08x}', 'bytes_including_terminator': ROW_TERMINATED_SIZE},
            {'vaddr': f'0x{ATLAS_ADDRESS:08x}', 'bytes': ATLAS_TUPLE_SIZE,
             'meaning': 'first all-zero 16-byte tuple terminates the atlas list'},
            {'vaddr': f'0x{DISPATCH_TABLE_ADDRESS + DISPATCH_SELECTOR * 4:08x}',
             'bytes': 4, 'meaning': 'type-8 dispatch target 0x0011e480'},
            *[{'vaddr': f'0x{address:08x}', 'bytes_including_nul': len(value),
               'string_id': string_id, 'key': key, 'bytes_hex': value.hex()}
              for string_id, key, address, value in strings],
        ],
        'output_bytes': len(output),
        'output_sha256': hashlib.sha256(output).hexdigest(),
        'record': {
            'type': EXPECTED_TYPE,
            'capacities': list(EXPECTED_CAPACITIES),
            'bindings': {'stream_id': 1, 'atlas_id': 1,
                         'constructor_handler_id': 1, 'update_handler_id': 1,
                         'type8_dispatch_handler_id': 1},
        },
        'binding_meanings': {
            'stream_id_1': 'source stream address 0x0024f6d0; initialized by 0x0011b518',
            'atlas_id_1': 'terminated tuple list at 0x001f8e98',
            'constructor_handler_id_1': 'source constructor 0x0011b518',
            'update_handler_id_1': 'source updater 0x0011c2c8',
            'type8_dispatch_handler_id_1': 'source table 0x001f8f48[8] target 0x0011e480',
        },
        'stream_initialization': [
            {'opcode': 14, 'reference': 'MenuTransform1f9c00', 'source_helper': '0x001215e0'},
            {'opcode': 6, 'reference': 'MenuTransform1f9c2c', 'source_helper': '0x001215e0'},
        ],
        'strings': [{'id': string_id, 'key': key, 'source_vaddr': f'0x{address:08x}',
                     'bytes_including_nul': len(value), 'bytes_hex': value.hex()}
                    for string_id, key, address, value in strings],
        'notes': [
            'The source stream address 0x0024f6d0 is not file-backed in the ELF; no guest pointer or memory image is serialized.',
            'Only the first zero atlas tuple is imported; consumer 0x0011f7a0 stops at that tuple.',
            'Literal bytes are preserved without conversion; strings use the game byte encoding.',
            'No save format, memory-card service behavior, or callback behavior is implemented by this artifact.',
        ],
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
        print(f'Restore data import failed: {exc}', file=sys.stderr)
        return 2
    print(json.dumps({'bytes': len(data), 'output_sha256': metadata['output_sha256'],
                      'source_sha256': metadata['source_sha256']}))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
