#!/usr/bin/env python3
"""Import the five source format strings used by results money rendering.

OKRTXT1\0 v1 has an 80-byte little-endian header (<8sII32s32s), followed by
five 64-byte NUL-terminated ASCII string slots. Slots are ordered by their ELF
addresses: zero prefix (0x223648), zero suffix (0x223658), grouped decimal
(0x223660), money summary (0x223670), and money display (0x223688). The
artifact carries the owned ELF hash and a digest of its padded string payload.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

from import_battle_data import ELF_SHA256, ElfImage

MAGIC = b'OKRTXT1\0'
VERSION = 1
STRING_ADDRESSES = (0x223648, 0x223658, 0x223660, 0x223670, 0x223688)
STRING_BYTES = 64
PAYLOAD_BYTES = len(STRING_ADDRESSES) * STRING_BYTES
HEADER = struct.Struct('<8sII32s32s')

def sha256(data):
    return hashlib.sha256(data).hexdigest()

def _read_string(image, address):
    raw = image.read(address, STRING_BYTES)
    terminator = raw.find(b'\0')
    if terminator < 0:
        raise ValueError(f'Unterminated results format string at {address:#x}')
    value = raw[:terminator]
    if not value:
        raise ValueError(f'Empty results format string at {address:#x}')
    try:
        value.decode('ascii')
    except UnicodeDecodeError as exc:
        raise ValueError(f'Non-ASCII results format string at {address:#x}') from exc
    return value + bytes(STRING_BYTES - len(value)), value

def extract_payload(image, source_digest):
    if not isinstance(source_digest, bytes) or len(source_digest) != 32:
        raise ValueError('Invalid source ELF digest')
    slots, metadata = [], []
    for address in STRING_ADDRESSES:
        padded, value = _read_string(image, address)
        slots.append(padded)
        metadata.append({
            'vaddr': f'0x{address:08x}',
            'bytes_including_nul': len(value) + 1,
            'text': value.decode('ascii'),
            'padded_slot_sha256': sha256(padded),
        })
    payload = b''.join(slots)
    return payload, {
        'schema': 'okage-results-text-import-v1',
        'elf_sha256': source_digest.hex(),
        'payload_sha256': sha256(payload),
        'output_bytes': HEADER.size + len(payload),
        'source_format_strings': metadata,
        'source_provenance': {
            'money': '0x00123c20',
            'money_summary': '0x00123a90',
            'string_substitution_helper': '0x001f15a8',
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
    artifact = HEADER.pack(MAGIC, VERSION, PAYLOAD_BYTES, digest,
                           hashlib.sha256(payload).digest()) + payload
    if len(artifact) != HEADER.size + PAYLOAD_BYTES:
        raise ValueError('Results text artifact has an unexpected extent')
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
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--manifest', type=Path)
    args = parser.parse_args(argv)
    manifest = args.manifest or args.output.with_name(args.output.name + '.manifest.json')
    try:
        artifact, metadata = import_elf(args.elf.read_bytes())
        write_outputs(args.output, manifest, artifact, metadata)
    except (OSError, ValueError, struct.error) as exc:
        print(f'results text import failed: {exc}', file=sys.stderr)
        return 2
    print(json.dumps({'bytes': len(artifact), 'artifact_sha256': metadata['artifact_sha256'],
                      'payload_sha256': metadata['payload_sha256']}))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
