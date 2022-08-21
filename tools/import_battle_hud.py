#!/usr/bin/env python3
"""Import source-authored battle HUD palette, threshold, and format strings.

OKBHUD2\0 v2 is a pointer-free 480-byte artifact. Its 80-byte header is
<8sII32s32s>: magic, version 2, payload size 400, owned ELF SHA256 bytes,
and payload SHA256 bytes. The payload contains 18 signed 32-bit palette words
from 0x247b50, the authored 64-bit threshold at 0x247b98, and five
NUL-terminated source format strings padded to 64 bytes, in source order
0x2236c8, 0x2236e0, 0x223688, 0x2236e8, 0x223700. The first three retain
v1's payload prefix; the last two add124200's resource-number formats. The
independent type3 threshold at0x248088 must match0x247b98 byte-for-byte.
Only each string through its first NUL is
copied; adjacent ELF bytes are not included in the padded slots.
"""
import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path

from import_battle_data import ELF_SHA256, ElfImage

MAGIC = b'OKBHUD2\0'
VERSION = 2
PALETTE_ADDRESS = 0x247b50
PALETTE_WORD_COUNT = 18
PALETTE_BYTES = PALETTE_WORD_COUNT * 4
THRESHOLD_ADDRESS = 0x247b98
TYPE3_THRESHOLD_ADDRESS = 0x248088
THRESHOLD_BYTES = 8
STRING_ADDRESSES = (0x2236c8, 0x2236e0, 0x223688, 0x2236e8, 0x223700)
STRING_BYTES = 64
PAYLOAD_BYTES = PALETTE_BYTES + THRESHOLD_BYTES + len(STRING_ADDRESSES) * STRING_BYTES
HEADER = struct.Struct('<8sII32s32s')
OUTPUT_BYTES = HEADER.size + PAYLOAD_BYTES
EXPECTED_THRESHOLD = 0.1

def sha256(data):
    return hashlib.sha256(data).hexdigest()

def _read_string(image, address):
    raw = image.read(address, STRING_BYTES)
    terminator = raw.find(b'\0')
    if terminator < 0:
        raise ValueError(f'Unterminated HUD format string at {address:#x} (limit {STRING_BYTES} bytes)')
    value = raw[:terminator]
    if not value:
        raise ValueError(f'Empty HUD format string at {address:#x}')
    try:
        value.decode('ascii')
    except UnicodeDecodeError as exc:
        raise ValueError(f'Non-ASCII HUD format string at {address:#x}') from exc
    return value + bytes(STRING_BYTES - len(value)), value

def extract_payload(image, source_digest):
    """Read bounded data through ElfImage-style read(address, size) calls."""
    if not isinstance(source_digest, bytes) or len(source_digest) != 32:
        raise ValueError('Invalid source ELF digest')
    palette = image.read(PALETTE_ADDRESS, PALETTE_BYTES)
    palette_words = struct.unpack('<18i', palette)
    threshold_raw = image.read(THRESHOLD_ADDRESS, THRESHOLD_BYTES)
    threshold, = struct.unpack('<d', threshold_raw)
    if not math.isfinite(threshold) or threshold != EXPECTED_THRESHOLD:
        raise ValueError(f'Unsupported HUD threshold at {THRESHOLD_ADDRESS:#x}: {threshold!r}')
    type3_threshold_raw = image.read(TYPE3_THRESHOLD_ADDRESS, THRESHOLD_BYTES)
    if type3_threshold_raw != threshold_raw:
        raise ValueError('Type3 HUD threshold differs from the shared imported threshold')
    padded_strings = []
    string_metadata = []
    for address in STRING_ADDRESSES:
        padded, value = _read_string(image, address)
        padded_strings.append(padded)
        string_metadata.append({
            'vaddr': f'0x{address:08x}',
            'bytes_including_nul': len(value) + 1,
            'text': value.decode('ascii'),
            'padded_slot_bytes': STRING_BYTES,
            'padded_slot_sha256': sha256(padded),
        })
    payload = palette + threshold_raw + b''.join(padded_strings)
    if len(payload) != PAYLOAD_BYTES:
        raise ValueError('HUD payload has an unexpected extent')
    return payload, {
        'schema': 'okage-battle-hud-import-v2',
        'elf_sha256': source_digest.hex(),
        'payload_sha256': sha256(payload),
        'output_bytes': HEADER.size + len(payload),
        'source_provenance': {
            'palette_consumer': '0x00162c60',
            'threshold_consumer': '0x00163698',
            'formatting_function': '0x001240c8',
            'resource_formatting_function': '0x00124200',
            'type3_refresh_consumer': '0x00167e18',
            'type3_update_consumer': '0x001684d0',
        },
        'sources': {
            'palette': {
                'vaddr': f'0x{PALETTE_ADDRESS:08x}',
                'bytes': PALETTE_BYTES,
                'sha256': sha256(palette),
                'signed_words': list(palette_words),
            },
            'threshold': {
                'vaddr': f'0x{THRESHOLD_ADDRESS:08x}',
                'bytes': THRESHOLD_BYTES,
                'sha256': sha256(threshold_raw),
                'value': threshold,
            },
            'type3_threshold': {
                'vaddr': f'0x{TYPE3_THRESHOLD_ADDRESS:08x}',
                'bytes': THRESHOLD_BYTES,
                'sha256': sha256(type3_threshold_raw),
                'value': threshold,
                'byte_identical_to': f'0x{THRESHOLD_ADDRESS:08x}',
            },
            'format_strings': string_metadata,
        },
    }

def extract_elf(data):
    digest = hashlib.sha256(data).digest()
    if digest.hex() != ELF_SHA256:
        raise ValueError('Owned ELF SHA256 does not match supported retail revision')
    return extract_payload(ElfImage(data), digest)

def import_elf(data):
    digest = hashlib.sha256(data).digest()
    payload, metadata = extract_elf(data)
    header = HEADER.pack(MAGIC, VERSION, PAYLOAD_BYTES, digest,
                         hashlib.sha256(payload).digest())
    artifact = header + payload
    if len(artifact) != OUTPUT_BYTES:
        raise ValueError('HUD artifact has an unexpected extent')
    metadata['artifact_sha256'] = sha256(artifact)
    return artifact, metadata

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
            json.dump(metadata, stream, indent=2, sort_keys=True)
            stream.write('\n')
        written.append(manifest)
    except OSError:
        for path in written:
            path.unlink(missing_ok=True)
        raise

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True,
                        help='Hash-pinned owned ELF32 file')
    parser.add_argument('--output', type=Path, required=True,
                        help='Fresh output binary path')
    parser.add_argument('--manifest', type=Path,
                        help='Fresh output metadata path (default: OUTPUT.manifest.json)')
    args = parser.parse_args(argv)
    manifest_path = args.manifest or args.output.with_name(args.output.name + '.manifest.json')
    try:
        artifact, metadata = import_elf(args.elf.read_bytes())
        write_outputs(args.output, manifest_path, artifact, metadata)
    except (OSError, ValueError, struct.error) as exc:
        print(f'battle HUD import failed: {exc}', file=sys.stderr)
        return 2
    print(json.dumps({'bytes': len(artifact), 'artifact_sha256': metadata['artifact_sha256'],
                      'payload_sha256': metadata['payload_sha256']}))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
