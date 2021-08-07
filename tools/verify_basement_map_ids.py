#!/usr/bin/env python3
"""Check native basement placement IDs against a completed original snapshot.

This is a read-only allocation/association audit, not a rendering comparison.
The snapshot is never a native runtime input. Hidden output caches and later
visibility changes are recorded, not used to overwrite native state.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import zipfile
import zlib

from inspect_disc import inspect_elf
from prepare_assets import ELF_HASH
from verify_event import digest, write_exact

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    elf = Path('local/extracted/SCUS_971.29')
    if digest(elf) != ELF_HASH:
        raise ValueError('Different ELF')
    data = elf.read_bytes()
    segments = inspect_elf(data)['segments']
    for s in segments:
        base = int(s['vaddr'], 16)
        if s['type'] == 1 and base <= 0x1b8260 < base + s['filesz']:
            instruction = struct.unpack_from('<I', data, s['offset'] + 0x1b8260 - base)[0]
            break
    else:
        raise ValueError('Missing scene reset instruction')
    if instruction != 0x24060280:
        raise ValueError('Scene reset no longer loads instance limit640')

    prefix = 'local/reference-home2c-basement-departure-600-v1'
    run = Path(prefix + '.run.json')
    metadata = json.loads(run.read_text())
    if metadata['exit_code'] != 0 or metadata['command'][2] != '600':
        raise ValueError('Incomplete reference')
    snapshot = Path(prefix + '.state.zip')
    with zipfile.ZipFile(snapshot) as archive:
        info = archive.getinfo('ram')
    if info.compress_type != 93 or info.file_size != 64 * 1024 * 1024:
        raise ValueError('Snapshot format')
    with snapshot.open('rb') as f:
        f.seek(info.header_offset)
        header = f.read(30)
        if header[:4] != b'PK\x03\x04':
            raise ValueError('ZIP header')
        name, extra = struct.unpack_from('<HH', header, 26)
        f.seek(name + extra, 1)
        compressed = f.read(info.compress_size)
    ram = subprocess.run(['zstd', '-dc'], input=compressed, capture_output=True, check=True).stdout
    if len(ram) != info.file_size or zlib.crc32(ram) != info.CRC:
        raise ValueError('Snapshot CRC')
    word = lambda at: struct.unpack_from('<I', ram, at)[0]
    half = lambda at: struct.unpack_from('<H', ram, at)[0]
    objects = word(0x21301c)
    if not 0 <= objects <= 32 * 1024 * 1024 - 640 * 60:
        raise ValueError('Instance table bounds')
    if (word(0x337b6c), word(0x337b70)) != (14, 633):
        raise ValueError('Changed original allocation endpoints')
    expected_ids = [0, 639, 638, 637, 636, 1, 635, 634, 633, 2, 3]
    associations = []
    for placement, instance in enumerate(expected_ids):
        obj = objects + instance * 60
        if half(obj + 8) != placement + 1 or half(obj + 4) != 3 + placement * 4:
            raise ValueError('Original model/channel association differs')
        associations.append(dict(placement=placement, instance=instance,
                                 model_handle=half(obj + 8), first_channel=half(obj + 4),
                                 visible=bool(word(obj) & 0x800)))
    game_map = Path('local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOMEB1B.XPF/a.xmp')
    artifacts = [elf, run, snapshot, game_map]
    commands = []
    for build in ['native-release', 'native', 'model-sanitize']:
        binary = Path('build') / build / 'okage-assets'
        output = a.output / (build + '.bin')
        command = [str(binary), 'map-visibility', str(game_map), str(output)]
        subprocess.run(command, check=True)
        records = list(struct.iter_unpack('<7I', output.read_bytes()))
        if [row[:2] for row in records] != list(enumerate(expected_ids)):
            raise ValueError('Native allocation differs: ' + build)
        if any(row[2:] != (1, 0, 1, 1, 0) for row in records):
            raise ValueError('Native static-map visibility lifecycle differs')
        commands.append(command)
        artifacts += [binary, output]
    report = dict(scope=__doc__, elf_sha256=ELF_HASH, scene_reset_address='0x1b8260',
                  instance_limit=640, original_front=14, original_back=633,
                  associations=associations, commands=commands,
                  sha256={str(path): digest(path) for path in artifacts},
                  limits=['Model/channel associations are specific to this completed basement snapshot.',
                          'Original final visibility differs from native initial map visibility by event design.',
                          'No full constructor, hidden channel, raster, audio or loading-timing equivalence claim.'])
    write_exact(a.output / 'report.json', (json.dumps(report, indent=2) + '\n').encode())
    print('All11 basement placement IDs match original model/channel associations in three builds')

if __name__ == '__main__':
    main()
