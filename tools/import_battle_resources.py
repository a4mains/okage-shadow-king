#!/usr/bin/env python3
"""Import owned first-battle resources from the supported retail ELF and disc.

The output directory contains battle-data.bin (OKBTDAT1), BTL.RUW,
battle-ui.bin (OKBTUI1), battle-camera.bin (OKBTCAM1), and battle-hud.bin
(OKBHUD2), plus battle-ui-commands.bin (OKBCMD1).
See import_battle_ui.py for the pointer-free authored interface schema.
Camera format v5 is little endian: header <8sII32s> = magic, version 5,
stage count 23, source ELF SHA256 bytes. It retains the v2 rows (stages 1..23,
initial index stage*8+0x5c and offsets 0 through 4), then the v3 exact global
table indices 0x28, 0x2b, 0x2c, 0x2d, and 0x2e for the source victory-camera
selector, then v4's 0x151e68 command-action indices 0x05, 0x07..0x0c, 0x32,
0x35, 0x37, 0x38, 0x3b, 0x3e, 0x41, 0x44 and 0x131, then v5's escape-parent
indices 0x29 (0x14b1f0 party escape) and 0x2a (0x14b348 enemy escape). Each row has three fixed-width slots: presence u32 (0 or 1), then
0x90 preset bytes; absent slots contain 0x90 zero bytes. Table pointers are
resolved during import and never appear in the artifact. One 0x90 preset
contains the three 0x30 camera lanes consumed by 0x14efa0/0x14f0d8. The parser
continues to accept legacy v1, v2, v3 and v4 artifacts.

All inputs are checked against the owned USA retail ELF/disc hashes and the
loose BTL.RUW file hash. The output directory must not already exist.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import tempfile
from pathlib import Path

from import_battle_data import ELF_SHA256, STAGE_COUNT, ElfImage, import_elf
from import_battle_ui import import_elf as import_ui_elf
from import_battle_hud import import_elf as import_hud_elf
from import_battle_ui_commands import import_elf as import_commands_elf
from inspect_disc import Disc

DISC_SHA256 = '08623a1400e2610aecd0247ae7440c97501495c6f819f675494e44de20b62d2a'
BTL_RUW_SHA256 = '2bc4de13d79919a53e7962250ea40acca28bb0e027ee750f35a87b6903dc8800'
CAMERA_MAGIC = b'OKBTCAM1'
CAMERA_VERSION = 5
CAMERA_TABLE = 0x235ea0
CAMERA_HEADER = struct.Struct('<8sII32s')
CAMERA_SLOT_SIZE = 4 + 0x90
CAMERA_SLOTS_PER_ROW = 3
CAMERA_ROWS_PER_STAGE = 5
CAMERA_INDEXED_ROWS = (0x28, 0x2b, 0x2c, 0x2d, 0x2e,
                       0x05, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x32, 0x35,
                       0x37, 0x38, 0x3b, 0x3e, 0x41, 0x44, 0x131,
                       0x29, 0x2a)
RUW_PATH = 'CMNDATA/WIN/BTL.RUW'

def sha256(data):
    return hashlib.sha256(data).hexdigest()

def extract_camera_tables(image, source_digest):
    """Build pointer-free camera table data from a hash-pinned ELF image."""
    if len(source_digest) != 32:
        raise ValueError('Invalid source ELF digest')
    result = bytearray(CAMERA_HEADER.pack(CAMERA_MAGIC, CAMERA_VERSION,
                                          STAGE_COUNT, source_digest))
    for stage in range(1, STAGE_COUNT + 1):
        initial_index = stage * 8 + 0x5c
        for row_index in range(initial_index, initial_index + CAMERA_ROWS_PER_STAGE):
            row_addr = CAMERA_TABLE + row_index * 12
            pointers = struct.unpack('<3I', image.read(row_addr, 12))
            for pointer in pointers:
                if pointer == 0:
                    result.extend(struct.pack('<I', 0))
                    result.extend(bytes(0x90))
                else:
                    if pointer & 3:
                        raise ValueError(f'Unaligned camera preset pointer {pointer:#x}')
                    preset = image.read(pointer, 0x90)
                    result.extend(struct.pack('<I', 1))
                    result.extend(preset)
    for row_index in CAMERA_INDEXED_ROWS:
        row_addr = CAMERA_TABLE + row_index * 12
        pointers = struct.unpack('<3I', image.read(row_addr, 12))
        for pointer in pointers:
            if pointer == 0:
                result.extend(struct.pack('<I', 0))
                result.extend(bytes(0x90))
            else:
                if pointer & 3:
                    raise ValueError(f'Unaligned camera preset pointer {pointer:#x}')
                preset = image.read(pointer, 0x90)
                result.extend(struct.pack('<I', 1))
                result.extend(preset)
    return bytes(result)

def extract_owned_ruw(disc_path):
    """Return the exact loose BTL.RUW bytes and ISO location from the owned disc."""
    path = Path(disc_path)
    if path.is_symlink() or not path.is_file():
        raise ValueError('Disc input must be an ordinary file, not a symlink')
    if sha256_file(path) != DISC_SHA256:
        raise ValueError('Disc SHA256 does not match supported owned USA image')
    disc = Disc(path)
    try:
        records = [entry for entry in disc.inventory()
                   if entry['path'].casefold() == RUW_PATH.casefold()]
        if len(records) != 1:
            raise ValueError(f'Expected exactly one {RUW_PATH} ISO file')
        entry = records[0]
        if entry['size'] != 19596:
            raise ValueError('Owned BTL.RUW size differs from supported resource')
        data = disc.read(entry['lba'], entry['size'])
    finally:
        disc.close()
    if sha256(data) != BTL_RUW_SHA256:
        raise ValueError('BTL.RUW SHA256 does not match supported owned resource')
    return data, entry

def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()

def _reject_symlink_path(path):
    absolute = Path(os.path.abspath(path))
    if absolute.is_symlink():
        raise ValueError(f'Refusing symlink output: {absolute}')
    parent = absolute.parent.resolve(strict=True)
    if not parent.is_dir():
        raise ValueError('Output parent directory must be a directory')
    return parent / absolute.name

def write_bundle(output, artifacts, manifest):
    """Create a new directory exclusively; never merge or replace an output."""
    target = _reject_symlink_path(output)
    if target.exists():
        raise ValueError('Output directory already exists; choose a fresh path')
    parent = target.parent
    if not parent.is_dir():
        raise ValueError('Output parent directory must already exist')
    temp = Path(tempfile.mkdtemp(prefix=f'.{target.name}.', dir=parent))
    created_target = False
    try:
        for name, data in artifacts.items():
            if Path(name).name != name or name in ('', '.', '..'):
                raise ValueError('Unsafe artifact name')
            with (temp / name).open('xb') as stream:
                stream.write(data)
        with (temp / 'manifest.json').open('xb') as stream:
            stream.write((json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode())
        # mkdir is exclusive; if another process won the path, preserve it.
        os.mkdir(target)
        created_target = True
        for child in temp.iterdir():
            os.rename(child, target / child.name)
        temp.rmdir()
    except BaseException:
        shutil.rmtree(temp, ignore_errors=True)
        if created_target and target.exists():
            shutil.rmtree(target, ignore_errors=True)
        raise

def import_resources(elf_data, disc_path):
    """Validate source inputs and return artifacts and a reproducible manifest."""
    elf_digest = hashlib.sha256(elf_data).digest()
    if elf_digest.hex() != ELF_SHA256:
        raise ValueError('Owned ELF SHA256 does not match supported retail revision')
    battle_data = import_elf(elf_data)
    image = ElfImage(elf_data)
    camera_data = extract_camera_tables(image, elf_digest)
    ui_data, ui_manifest = import_ui_elf(elf_data)
    hud_data, hud_manifest = import_hud_elf(elf_data)
    commands_data, commands_manifest = import_commands_elf(elf_data)
    ruw_data, ruw_entry = extract_owned_ruw(disc_path)
    artifacts = {
        'battle-data.bin': battle_data,
        'BTL.RUW': ruw_data,
        'battle-camera.bin': camera_data,
        'battle-ui.bin': ui_data,
        'battle-hud.bin': hud_data,
        'battle-ui-commands.bin': commands_data,
    }
    manifest = {
        'format': 'okage-owned-battle-resources-v1',
        'sources': {
            'elf_sha256': elf_digest.hex(),
            'disc_sha256': DISC_SHA256,
            'btl_ruw': {
                'iso_path': ruw_entry['path'],
                'lba': ruw_entry['lba'],
                'size': ruw_entry['size'],
                'sha256': sha256(ruw_data),
            },
        },
        'artifacts': {
            name: {'size': len(data), 'sha256': sha256(data)}
            for name, data in artifacts.items()
        },
        'camera_format': {
            'magic': CAMERA_MAGIC.decode('ascii'),
            'version': CAMERA_VERSION,
            'stage_count': STAGE_COUNT,
            'stage_row_indices': 'stage*8+0x5c through +4; each row has 3 optional 0x90-byte presets with u32 presence',
            'indexed_row_indices': list(CAMERA_INDEXED_ROWS),
        },
        'ui_format': ui_manifest,
        'hud_format': hud_manifest,
        'commands_format': commands_manifest,
    }
    return artifacts, manifest

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True, help='Hash-pinned owned ELF32 file')
    parser.add_argument('--disc', type=Path, required=True, help='Hash-pinned owned ISO/PS2 disc image')
    parser.add_argument('--output-dir', type=Path, required=True, help='New output directory')
    args = parser.parse_args(argv)
    for path in (args.elf, args.disc):
        if path.is_symlink() or not path.is_file():
            parser.error(f'input must be an ordinary file: {path}')
    artifacts, manifest = import_resources(args.elf.read_bytes(), args.disc)
    write_bundle(args.output_dir, artifacts, manifest)
    print(f"Imported {len(artifacts)} assets to {args.output_dir}")

if __name__ == '__main__':
    main()
