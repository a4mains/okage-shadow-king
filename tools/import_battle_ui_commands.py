#!/usr/bin/env python3
"""Import authored command-panel constants and shared UI defaults.

OKBCMD2\0 is a pointer-free 384-byte artifact (native retains v1 support). Its 80-byte header is
<8sII32s32s> (magic, version, payload size, pinned ELF SHA256, payload SHA256).
The 64-byte payload is three little-endian signed-int tables in source order:
two words at 0x247ba0, two at 0x247ba8, four at 0x247bb0; then four words
from each authored shared default at 0x200b78 (metrics) and 0x200b88 (clip).
The v2 suffix adds four RGB triples at247ae8 and three NUL-terminated strings
from247878/247b18/247b38, each padded to64 bytes.
The last two default records are ELF-authored initial contents only; source pointers
refer to the shared globals, and this import does not assert runtime immutability.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from import_battle_data import ELF_SHA256, ElfImage

MAGIC = b'OKBCMD2\0'
VERSION = 2
HEADER = struct.Struct('<8sII32s32s')
PAYLOAD_BYTES = 304
OUTPUT_BYTES = HEADER.size + PAYLOAD_BYTES
TABLES = ((0x247ba0, 2, 'category_zero'),
          (0x247ba8, 2, 'category_nonzero'),
          (0x247bb0, 4, 'category_selection'))
DEFAULTS = ((0x200b78, 4, 'metrics'), (0x200b88, 4, 'clip'))

def sha256(data):
    return hashlib.sha256(data).hexdigest()

def extract_payload(image, source_digest):
    if not isinstance(source_digest, bytes) or len(source_digest) != 32:
        raise ValueError('Invalid source ELF digest')
    payload = bytearray()
    tables = {}
    defaults = {}
    for address, count, name in TABLES:
        raw = image.read(address, count * 4)
        if len(raw) != count * 4:
            raise ValueError(f'Truncated source table at {address:#x}')
        words = struct.unpack('<' + 'i' * count, raw)
        tables[name] = {'vaddr': f'0x{address:08x}', 'bytes': len(raw),
                        'sha256': sha256(raw), 'signed_words': list(words)}
        payload.extend(raw)
    for address, count, name in DEFAULTS:
        raw = image.read(address, count * 4)
        if len(raw) != count * 4:
            raise ValueError(f'Truncated shared default at {address:#x}')
        words = struct.unpack('<' + 'i' * count, raw)
        defaults[name] = {'vaddr': f'0x{address:08x}', 'bytes': len(raw),
                          'sha256': sha256(raw), 'signed_words': list(words),
                          'ownership': 'authored ELF initial contents; runtime global may mutate'}
        payload.extend(raw)
    palette = image.read(0x247ae8, 48)
    if len(palette) != 48:
        raise ValueError('Truncated target palette')
    payload.extend(palette)
    target_sources = {'palette': {'vaddr': '0x00247ae8', 'bytes': 48, 'sha256': sha256(palette)}, 'formats': []}
    for address in (0x247878, 0x247b18, 0x247b38):
        raw = image.read(address, 64)
        end = raw.find(b'\0')
        if len(raw) != 64 or end < 0:
            raise ValueError('Target format extent/terminator')
        text = raw[:end]
        if (address == 0x247878 and b'%' in text) or (address != 0x247878 and (text.count(b'%') != 1 or b'%s' not in text)):
            raise ValueError('Unrecovered target format conversion')
        padded = text + bytes(64-len(text))
        payload.extend(padded)
        target_sources['formats'].append({'vaddr': hex(address), 'source_string_sha256': sha256(raw[:end+1]), 'padded_sha256': sha256(padded)})
    if len(payload) != PAYLOAD_BYTES:
        raise ValueError('Unexpected command data payload extent')
    # Pin the observed authored values so a different table revision fails closed.
    expected = ((5, 19), (20, 6), (2, 0, 4, 6), (12, 24, 6, 18), (-1000, 0, 512, 2048))
    actual = tuple(tuple(x['signed_words']) for x in (*tables.values(), *defaults.values()))
    if actual != expected:
        raise ValueError('Command tables or UI defaults differ from the supported authored values')
    return bytes(payload), {
        'schema': 'okage-battle-ui-command-import-v2',
        'elf_sha256': source_digest.hex(),
        'payload_sha256': sha256(payload),
        'output_bytes': HEADER.size + len(payload),
        'source_provenance': {
            'consumer': '0x00164090',
            'command_tables': '0x00247ba0, 0x00247ba8, 0x00247bb0',
            'shared_default_references': '0x001a9a48 installs 0x00200b78 and 0x00200b88 in embedded command stream',
        },
        'target_sources': target_sources,
        'tables': tables,
        'shared_defaults': defaults,
    }

def import_elf(data):
    digest = hashlib.sha256(data).digest()
    if digest.hex() != ELF_SHA256:
        raise ValueError('Owned ELF SHA256 does not match supported retail revision')
    payload, metadata = extract_payload(ElfImage(data), digest)
    artifact = HEADER.pack(MAGIC, VERSION, PAYLOAD_BYTES, digest,
                           hashlib.sha256(payload).digest()) + payload
    if len(artifact) != OUTPUT_BYTES:
        raise ValueError('Command data artifact has unexpected extent')
    metadata['artifact_sha256'] = sha256(artifact)
    return artifact, metadata

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--manifest', type=Path)
    args = parser.parse_args(argv)
    manifest = args.manifest or args.output.with_name(args.output.name + '.manifest.json')
    paths = (args.output, manifest)
    for path in paths:
        if path.is_symlink() or path.exists():
            raise ValueError(f'Refusing symlink or existing output: {path}')
    artifact, metadata = import_elf(args.elf.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    manifest.parent.mkdir(parents=True, exist_ok=True)
    written = []
    try:
        with args.output.open('xb') as stream:
            stream.write(artifact)
        written.append(args.output)
        with manifest.open('x', encoding='utf-8') as stream:
            json.dump(metadata, stream, indent=2, sort_keys=True)
            stream.write('\n')
        written.append(manifest)
    except OSError:
        for path in written:
            path.unlink(missing_ok=True)
        raise
    return 0

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, struct.error) as exc:
        raise SystemExit(f'import_battle_ui_commands: {exc}')
