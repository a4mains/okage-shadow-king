#!/usr/bin/env python3
"""Import the pointer-free ordinary field message window rows from the owned ELF.

Opcode 4c (0x130550) and field manager phase 8 build 0x11f570(manager, 0x2293e0,
0, id). Its descriptor 0x2234e0 constructs one layer per row of configuration
0x2293e0 until a row whose stream word is zero (0x11f668):

* row 0x2293e0: type 0 (controller 0x148958), constructor 0x147ec8 (stream
  0x2dbb40 = [14, 0x1f9c00, 6, 0x1f9c2c]), updater 0x147fe0, capacities 16/8;
* row 0x229450: type 1 (controller 0x148dc8), constructor 0x1468c8 (stream
  0x2dbc40 = [14, 0x1f9c00, 6, 0x1f9c1c]), updater 0x146ee8, capacities 12/2.

OKMSW1\\0\\0 v1 is an 88-byte little-endian header, both 28-word rows (stream,
atlas, constructor and updater words rewritten to logical IDs), the rows'
shared terminated atlas list 0x1fcdb0, and the literal constants the
controllers/updaters read: cursor selectors 0x229010 (3 words), the
0x147ec8 period table 0x229040 (4 words), the 0x1f9c1c/0x1f9c2c period words,
and the NUL-inclusive strings 0x228ff8 (" "), 0x229000 ("NULL") and 0x229008
("system"). No guest pointer, callback behavior or stream state is serialized.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

from import_battle_data import ElfImage

ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
SOURCE_SHA256 = '9290a28fd31947bbf4827c8adae171544246df6dbdab966f4b2b3110123a3c8e'
ROWS_ADDRESS = 0x2293e0
ROW_SIZE = 112
ROW_COUNT = 2
ATLAS_ADDRESS = 0x1fcdb0
ATLAS_TUPLE_SIZE = 16
ATLAS_MAX_TUPLES = 32
DISPATCH_TABLE_ADDRESS = 0x1f8f48
DISPATCH = {0: 0x148958, 1: 0x148dc8}
SELECTORS_ADDRESS = 0x229010
PERIODS_ADDRESS = 0x229040
PERIOD_WORDS_ADDRESS = 0x1f9c1c
STRINGS = ((0x228ff8, b' \0'), (0x229000, b'NULL\0'), (0x229008, b'system\0'))
MAGIC = b'OKMSW1\0\0'
VERSION = 1
HEADER = struct.Struct('<8sIIII32s32s')
ROW = struct.Struct('<28I')

EXPECTED = (
    {'stream': 0x2dbb40, 'ctor': 0x147ec8, 'updater': 0x147fe0, 'type': 0,
     'capacities': (16, 8), 'layout': 0, 'stream_id': 1, 'ctor_id': 1, 'updater_id': 1},
    {'stream': 0x2dbc40, 'ctor': 0x1468c8, 'updater': 0x146ee8, 'type': 1,
     'capacities': (12, 2), 'layout': 0, 'stream_id': 2, 'ctor_id': 2, 'updater_id': 2},
)

def extract_source(image):
    rows_raw = image.read(ROWS_ADDRESS, ROW_SIZE * ROW_COUNT + 4)
    if rows_raw[ROW_SIZE * ROW_COUNT:] != bytes(4):
        raise ValueError('Message configuration has no zero stream word after two rows')
    rows = []
    for index, expected in enumerate(EXPECTED):
        words = ROW.unpack(rows_raw[index * ROW_SIZE:(index + 1) * ROW_SIZE])
        if (words[0], words[21], words[22], words[23]) != (expected['stream'], ATLAS_ADDRESS,
                                                          expected['ctor'], expected['updater']):
            raise ValueError(f'Unsupported message row {index} bindings')
        if words[24] != expected['type'] or words[25:27] != expected['capacities']:
            raise ValueError(f'Unsupported message row {index} type/capacities')
        if words[20] != expected['layout'] or words[27] != 0:
            raise ValueError(f'Unsupported message row {index} layout flag/reserved word')
        rows.append(words)
    atlas = []
    for index in range(ATLAS_MAX_TUPLES):
        raw = image.read(ATLAS_ADDRESS + index * ATLAS_TUPLE_SIZE, ATLAS_TUPLE_SIZE)
        atlas.append(raw)
        if raw == bytes(ATLAS_TUPLE_SIZE):
            break
    else:
        raise ValueError('Message atlas has no terminator within 32 tuples')
    if len(atlas) != 1:
        raise ValueError('Unsupported message atlas entries before terminator')
    dispatch = b''
    for selector, handler in DISPATCH.items():
        raw = image.read(DISPATCH_TABLE_ADDRESS + selector * 4, 4)
        if struct.unpack('<I', raw)[0] != handler:
            raise ValueError(f'Unsupported type-{selector} message controller')
        dispatch += raw
    selectors = image.read(SELECTORS_ADDRESS, 16)
    if struct.unpack('<4I', selectors) != (0x27, 0x28, 0x29, 0):
        raise ValueError('Unsupported message cursor selector table')
    periods = image.read(PERIODS_ADDRESS, 16)
    if struct.unpack('<4i', periods) != (-4, 1, 2, 0):
        raise ValueError('Unsupported message period table')
    period_words = image.read(PERIOD_WORDS_ADDRESS, 20)
    if struct.unpack('<5I', period_words)[0] != 0xfffffffe or struct.unpack('<5I', period_words)[4] != 0xfffffc18:
        raise ValueError('Unsupported constructor period words')
    strings = b''
    for address, literal in STRINGS:
        raw = image.read(address, len(literal))
        if raw != literal:
            raise ValueError(f'Unsupported message string at 0x{address:08x}')
        strings += raw
    source = rows_raw + b''.join(atlas) + dispatch + selectors + periods + period_words + strings
    return source, rows, atlas, selectors, periods, period_words, strings

def import_elf(data):
    digest = hashlib.sha256(data).digest()
    if digest.hex() != ELF_SHA256:
        raise ValueError('Owned ELF SHA256 does not match supported retail revision')
    source, rows, atlas, selectors, periods, period_words, strings = extract_source(ElfImage(data))
    source_digest = hashlib.sha256(source).digest()
    if source_digest.hex() != SOURCE_SHA256:
        raise ValueError('Message window source literals SHA256 differ from supported literals: ' + source_digest.hex())
    payload = bytearray()
    for words, expected in zip(rows, EXPECTED):
        mapped = list(words)
        mapped[0], mapped[21] = expected['stream_id'], 1
        mapped[22], mapped[23] = expected['ctor_id'], expected['updater_id']
        payload.extend(ROW.pack(*mapped))
    payload.extend(b''.join(atlas))
    payload.extend(selectors[:12])
    payload.extend(periods)
    payload.extend(struct.pack('<2I', struct.unpack('<5I', period_words)[0], struct.unpack('<5I', period_words)[4]))
    payload.extend(strings)
    output = HEADER.pack(MAGIC, VERSION, ROW_COUNT, 28, len(atlas), digest, source_digest) + payload
    metadata = {
        'schema': 'okage-message-windows-import-v1',
        'scope': 'Pointer-free 0x2293e0/0x229450 message window rows, terminated atlas 0x1fcdb0, '
                 'controller dispatch checks and controller/updater literals; no callback behavior.',
        'elf_sha256': digest.hex(),
        'source_sha256': source_digest.hex(),
        'source_bytes': len(source),
        'rows': [{'vaddr': f'0x{ROWS_ADDRESS + i * ROW_SIZE:08x}', 'type': e['type'],
                  'capacities': list(e['capacities']), 'stream_id': e['stream_id'],
                  'source_stream': f"0x{e['stream']:08x}", 'source_constructor': f"0x{e['ctor']:08x}",
                  'source_updater': f"0x{e['updater']:08x}"} for i, e in enumerate(EXPECTED)],
        'constants': {'cursor_selectors_0x229010': [0x27, 0x28, 0x29],
                      'period_table_0x229040': [-4, 1, 2, 0],
                      'period_word_0x1f9c1c': '0xfffffffe', 'period_word_0x1f9c2c': '0xfffffc18',
                      'strings': {'0x228ff8': ' ', '0x229000': 'NULL', '0x229008': 'system'}},
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
        print(f'Message window import failed: {exc}', file=sys.stderr)
        return 2
    print(json.dumps({'bytes': len(data), 'output_sha256': metadata['output_sha256'],
                      'source_sha256': metadata['source_sha256']}))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
