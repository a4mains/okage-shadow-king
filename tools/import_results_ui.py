#!/usr/bin/env python3
"""Import the authored results interface literals as pointer-free data.

OKRSUI1\0 v1 has an 88-byte header (<8sIIII32s32s), followed by one 28-word
interface row, 16 widget IDs, 49 four-word animation records, and two blocks of
four stream constants. The source digest covers the terminated interface row,
widget IDs, animation bytes, and both stream-constant blocks in that order.

The interface row's stream, atlas, constructor, and updater addresses become
logical binding ID 1. Animation records remain literal four-word records; this
format does not assign meanings to their fields or execute them.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

from import_battle_data import ElfImage

ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
SOURCE_SHA256 = '9307f0adb6f8183153095634ccf9ac284d2d9bc1ed320d13ea050099af7aa183'
MAGIC = b'OKRSUI1\0'
VERSION = 1
ROW_ADDRESS = 0x248690
ROW_SIZE = 112
ROW_TERMINATED_SIZE = ROW_SIZE + 4
WIDGET_ADDRESS = 0x248250
WIDGET_SIZE = 64
ANIMATION_ADDRESS = 0x248290
ANIMATION_SIZE = 0x310
ANIMATION_SPANS = (224, 384, 96, 80)
ANIMATION_COUNTS = tuple(size // 16 for size in ANIMATION_SPANS)
STREAM_CONSTANT_SPANS = ((0x1f9c00, 16), (0x1f9c2c, 16))
HEADER = struct.Struct('<8sIIII32s32s')
ROW = struct.Struct('<28I')
WIDGETS = struct.Struct('<16I')
ANIMATION_RECORD = struct.Struct('<4I')
STREAM_CONSTANTS = struct.Struct('<4I')

# The source row contains guest stream, atlas, constructor, and updater addresses.
ROW_BINDINGS = ((0, 0x2e1930), (21, 0x1fddd8), (22, 0x169900), (23, 0x16b6c8))
WIDGET_IDS = (0x40, 0x41, 0x42, 0x43, 0x72, 0x73, 0x74, 0x75,
              0x6e, 0x6f, 0x70, 0x71, 0x76, 0x77, 0x78, 0x79)
STREAM_CONSTANT_VALUES = ((12, 24, 6, 18), (0xfffffc18, 10000, 0, 0x3f800000))
ANIMATION_END = (0xffffffff, 0xffffffff, 0, 0)

def extract_source(image):
    """Return validated source bytes, row words, and literal table records."""
    row_raw = image.read(ROW_ADDRESS, ROW_TERMINATED_SIZE)
    if row_raw[ROW_SIZE:] != bytes(4):
        raise ValueError('Results interface row terminator is not zero')
    row = ROW.unpack(row_raw[:ROW_SIZE])
    for index, address in ROW_BINDINGS:
        if row[index] != address:
            raise ValueError(f'Unsupported results interface binding at word {index}')
    if row[1:4] != (44, 40, 1):
        raise ValueError('Unsupported results interface constructor arguments')
    if row[24] != 11 or row[25:27] != (33, 17) or row[27] != 0:
        raise ValueError('Unsupported results interface type, capacities, or reserved word')

    widget_raw = image.read(WIDGET_ADDRESS, WIDGET_SIZE)
    widgets = WIDGETS.unpack(widget_raw)
    if widgets != WIDGET_IDS:
        raise ValueError('Unsupported results widget ID table')

    animation_raw = image.read(ANIMATION_ADDRESS, ANIMATION_SIZE)
    records = tuple(ANIMATION_RECORD.unpack_from(animation_raw, offset)
                    for offset in range(0, ANIMATION_SIZE, 16))
    offset = 0
    for table_index, (span_size, record_count) in enumerate(zip(ANIMATION_SPANS, ANIMATION_COUNTS)):
        table = records[offset:offset + record_count]
        if table_index < 3 and table[-1] != ANIMATION_END:
            raise ValueError(f'Results animation table {table_index} terminator differs')
        if table_index < 3 and ANIMATION_END in table[:-1]:
            raise ValueError(f'Results animation table {table_index} has an early terminator')
        if any(record[1] not in (0, 1, 2) for record in table if record != ANIMATION_END):
            raise ValueError(f'Results animation table {table_index} has an unsupported operation type')
        offset += record_count

    stream_raw = b''.join(image.read(address, size) for address, size in STREAM_CONSTANT_SPANS)
    streams = tuple(STREAM_CONSTANTS.unpack_from(stream_raw, index * 16) for index in range(2))
    if streams != STREAM_CONSTANT_VALUES:
        raise ValueError('Unsupported results stream constants')

    raw = row_raw + widget_raw + animation_raw + stream_raw
    return raw, row, widgets, records, streams

def import_elf(data):
    digest = hashlib.sha256(data).digest()
    if digest.hex() != ELF_SHA256:
        raise ValueError('Owned ELF SHA256 does not match supported retail revision')
    raw, row, widgets, records, streams = extract_source(ElfImage(data))
    source_digest = hashlib.sha256(raw).digest()
    if source_digest.hex() != SOURCE_SHA256:
        raise ValueError('Results UI source bytes SHA256 differ from supported literals')

    mapped = list(row)
    for index, _ in ROW_BINDINGS:
        mapped[index] = 1
    payload = (ROW.pack(*mapped) + WIDGETS.pack(*widgets) +
               b''.join(ANIMATION_RECORD.pack(*record) for record in records) +
               b''.join(STREAM_CONSTANTS.pack(*stream) for stream in streams))
    header = HEADER.pack(MAGIC, VERSION, 28, len(widgets), len(records), digest, source_digest)
    output = header + payload
    metadata = {
        'schema': 'okage-results-ui-import-v1',
        'elf_sha256': digest.hex(),
        'source_sha256': source_digest.hex(),
        'source_ranges': [
            {'vaddr': f'0x{ROW_ADDRESS:08x}', 'bytes_including_terminator': ROW_TERMINATED_SIZE},
            {'vaddr': f'0x{WIDGET_ADDRESS:08x}', 'bytes': WIDGET_SIZE},
            {'vaddr': f'0x{ANIMATION_ADDRESS:08x}', 'bytes': ANIMATION_SIZE,
             'span_bytes': list(ANIMATION_SPANS)},
            {'vaddr': f'0x{STREAM_CONSTANT_SPANS[0][0]:08x}', 'bytes': 16},
            {'vaddr': f'0x{STREAM_CONSTANT_SPANS[1][0]:08x}', 'bytes': 16},
        ],
        'output_sha256': hashlib.sha256(output).hexdigest(),
        'bindings': {'row_word_0': 1, 'row_word_21': 1,
                     'constructor_handler_id': 1, 'updater_handler_id': 1},
        'animation_table_record_counts': list(ANIMATION_COUNTS),
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
        print(f'results UI import failed: {exc}', file=sys.stderr)
        return 2
    print(json.dumps({'bytes': len(data), 'output_sha256': metadata['output_sha256'],
                      'source_sha256': metadata['source_sha256']}))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
