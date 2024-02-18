#!/usr/bin/env python3
"""Verify title assets and optionally import title streaming audio from owned USA media.

This records provenance for the eight authored SCR.XPF images, NIGHT_V/a.xmd,
and direct THEMA_1.XWH/XWB disc files. It does not decode or play audio.
"""
import argparse
import hashlib
import json
from pathlib import Path

from inspect_disc import Disc
from prepare_assets import ELF_HASH
from verify_event import write_exact

DISC_SHA256 = '08623a1400e2610aecd0247ae7440c97501495c6f819f675494e44de20b62d2a'
ELF_STRING_ADDRESSES = {
    0x227958: b'snddata/str/thema_1',
    0x227cd8: b'%s.xwh',
    0x227ce0: b'%s.xwb',
}
AUDIO = (
    {'path': 'SNDDATA/STR/THEMA_1.XWH', 'lba': 163585, 'size': 112},
    {'path': 'SNDDATA/STR/THEMA_1.XWB', 'lba': 163586, 'size': 23186288},
)
TITLE_ASSETS = (
    ('SCR.XPF', 'title_sample4.xim', 313392, '6accf8e52612c27a71e3e0802651d15d645c2ea4adcf19d60f5ddc42135910f1'),
    ('SCR.XPF', 'push.xim', 4528, '765fe72e8c1c7fbdc88e768bcf740fee71a07ed2bfa0f3ccc9528b4ebe116dfd'),
    ('SCR.XPF', 'new.xim', 2864, 'd3d28db8ecbee71fcdbaa2ba518385d8d18b4d6364db4e9665ccc7858678c84a'),
    ('SCR.XPF', 'restore.xim', 3632, 'ddfa4684d506dc8f9b6bc8c4f9c3cb62a128ca5adda7a5774d17d8220553c0a4'),
    ('SCR.XPF', 'maru_w.xim', 1328, 'cc174203e85424515a59435fbcdc81e899f64bcf712d56365a7caa7c62a986be'),
    ('SCR.XPF', 'maru_b.xim', 1328, '2a44c8f9c350dd52713423dc9b96d9bfb0924388f7ea05e9b9bbcb4504d33dbe'),
    ('SCR.XPF', 'sp.xim', 2480, '7ca4a33a628d2290a933fffbbff1506dfb1c9fbc180c458558c67479fcf4f0c9'),
    ('SCR.XPF', 'hanten.xim', 38960, '3f7abd54b29abd252cad43b98126ff43a42d07cd3cac2cf40879f002e81aec46'),
    ('NIGHT_V.XPF', 'a.xmd', 1396704, '34d378efccdfc6ec0af9bce8123d0fbe9e137e9a867b44332728f3457f5d5657'),
)

def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()

def sha256_file(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def address_bytes(elf, segments, address, size):
    if size < 0:
        raise ValueError('Negative ELF string extent')
    for segment in segments:
        base = int(segment['vaddr'], 16)
        file_end = base + segment['filesz']
        if segment['type'] == 1 and base <= address and address + size <= file_end:
            offset = segment['offset'] + address - base
            return elf[offset:offset + size]
    raise ValueError(f'ELF address outside file-backed load segments: 0x{address:x}')

def verify_title_strings(elf, segments):
    strings = {}
    for address, expected in ELF_STRING_ADDRESSES.items():
        actual = address_bytes(elf, segments, address, len(expected) + 1)
        if actual != expected + b'\0':
            raise ValueError(f'Unexpected title resource string at 0x{address:x}')
        strings[f'0x{address:06x}'] = actual[:-1].decode('ascii')
    return strings

def index_disc_files(report):
    entries = {}
    for entry in report.get('files', []):
        path = entry.get('path')
        if not isinstance(path, str) or path in entries:
            raise ValueError('Missing or duplicate disc path in index')
        entries[path] = entry
    return entries

def index_xpf_members(report):
    if report.get('elf_sha256') != ELF_HASH or report.get('image_sha256') != DISC_SHA256:
        raise ValueError('XPF verification provenance is not the pinned USA input')
    if report.get('all_corpus') is not True:
        raise ValueError('XPF verification does not cover the complete corpus')
    members = {}
    for row in report.get('results', []):
        archive, member = row.get('archive'), row.get('member')
        if member is None:
            continue
        key = (archive, member)
        if key in members:
            raise ValueError(f'Duplicate XPF member record: {archive}/{member}')
        members[key] = row
    return members

def verify_decoded_assets(decoded_root, verification):
    members = index_xpf_members(verification)
    result = []
    for archive_name, member_name, expected_size, expected_hash in TITLE_ASSETS:
        archive = f'CMNDATA/2D/{archive_name}'
        row = members.get((archive, member_name))
        if row is None or row.get('size') != expected_size or row.get('sha256') != expected_hash:
            raise ValueError(f'Missing or changed verified XPF member: {archive}/{member_name}')
        path = Path(decoded_root) / 'CMNDATA' / '2D' / archive_name / member_name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'Missing or unsafe decoded title asset: {path}')
        size = path.stat().st_size
        sha = sha256_file(path)
        if size != expected_size or sha != expected_hash:
            raise ValueError(f'Decoded title asset differs from verified member: {path}')
        result.append({'archive': archive, 'member': member_name, 'path': str(path),
                       'bytes': size, 'sha256': sha,
                       'verification': {'packed_size': row['packed_size'], 'consumed': row['consumed']}})
    return result

def read_audio_resources(disc, entries):
    result = []
    payloads = []
    for spec in AUDIO:
        entry = entries.get(spec['path'])
        if entry is None or entry.get('lba') != spec['lba'] or entry.get('size') != spec['size']:
            raise ValueError(f'Owned disc index differs for {spec["path"]}')
        data = disc.read(spec['lba'], spec['size'])
        if len(data) != spec['size']:
            raise ValueError(f'Truncated direct stream resource: {spec["path"]}')
        row = dict(spec, sha256=sha256_bytes(data))
        if spec['path'].endswith('.XWH'):
            row['header_hex'] = data.hex()
        result.append(row)
        payloads.append((spec['path'], data))
    return result, payloads

def install_audio_resources(audio_root, payloads):
    targets = [(Path(audio_root) / Path(relative).name, data) for relative, data in payloads]
    for target, data in targets:
        if target.is_symlink():
            raise RuntimeError('Refusing symlink output: ' + str(target))
        if target.exists() and target.read_bytes() != data:
            raise RuntimeError('Refusing changed output: ' + str(target))
    installed = []
    for target, data in targets:
        target.parent.mkdir(parents=True, exist_ok=True)
        write_exact(target, data)
        installed.append({'path': str(target), 'bytes': len(data), 'sha256': sha256_bytes(data)})
    return installed

def audit_title_resources(disc_path, disc_report_path, elf_path, xpf_verification_path,
                          decoded_root, audio_root, install_audio=False, disc_factory=Disc):
    disc_report = json.loads(Path(disc_report_path).read_text())
    if disc_report.get('image_sha256') != DISC_SHA256:
        raise ValueError('Disc index is not for the pinned owned USA image')
    if disc_report.get('elf', {}).get('sha256') != ELF_HASH:
        raise ValueError('Disc index ELF does not match the pinned USA ELF')
    actual_disc_hash = sha256_file(disc_path)
    if actual_disc_hash != DISC_SHA256:
        raise ValueError('Disc image hash differs from the pinned owned USA revision')
    elf = Path(elf_path).read_bytes()
    if sha256_bytes(elf) != ELF_HASH:
        raise ValueError('Different owned ELF')
    # inspect_elf performs the structural MIPS ELF bounds checks used by other importers.
    from inspect_disc import inspect_elf
    elf_info = inspect_elf(elf)
    strings = verify_title_strings(elf, elf_info['segments'])
    verification = json.loads(Path(xpf_verification_path).read_text())
    assets = verify_decoded_assets(decoded_root, verification)
    entries = index_disc_files(disc_report)
    disc = disc_factory(disc_path)
    try:
        audio, payloads = read_audio_resources(disc, entries)
    finally:
        disc.close()
    installed = install_audio_resources(audio_root, payloads) if install_audio else []
    return {
        'schema': 'okage-native-title-resource-import-v1',
        'scope': "Pinned source/resource verification; XWH/XWB payload inventory and optional direct audio installation.",
        'disc': {'path': str(disc_path), 'sha256': actual_disc_hash,
                 'disc_index': str(disc_report_path), 'elf_sha256': ELF_HASH},
        'elf_strings': strings,
        'xpf_verification': {'path': str(xpf_verification_path),
                             'native_binary_sha256': verification['native_binary_sha256'],
                             'archives': verification['archives'], 'members': verification['members']},
        'title_assets': assets,
        'audio_resources': audio,
        'audio_installed': bool(install_audio),
        'installed_audio': installed,
    }

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc', type=Path, default=Path('local/disc/Okage - Shadow King (USA).bin'))
    parser.add_argument('--disc-report', type=Path, default=Path('docs/disc-report.json'))
    parser.add_argument('--elf', type=Path, default=Path('local/extracted/SCUS_971.29'))
    parser.add_argument('--xpf-verification', type=Path, default=Path('local/xpf-verification.json'))
    parser.add_argument('--decoded-root', type=Path, default=Path('local/xpf-decoded'))
    parser.add_argument('--audio-root', type=Path, default=Path('local/audio-input/SNDDATA/STR'))
    parser.add_argument('--output', type=Path, required=True, help='Fresh ignored JSON provenance report')
    parser.add_argument('--install-audio', action='store_true',
                        help='Install direct XWH/XWB files after all provenance checks pass; existing different files are refused')
    args = parser.parse_args()
    if args.output.is_symlink() or args.output.exists():
        raise RuntimeError('Refusing existing title import report: ' + str(args.output))
    report = audit_title_resources(args.disc, args.disc_report, args.elf,
                                   args.xpf_verification, args.decoded_root,
                                   args.audio_root, args.install_audio)
    output = args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    write_exact(output, (json.dumps(report, indent=2) + '\n').encode())
    print(f"Verified {len(report['title_assets'])} title assets and {len(report['audio_resources'])} stream files")
    if args.install_audio:
        print(f'Installed audio files under {args.audio_root}')
    print(f'Report: {output}')

if __name__ == '__main__':
    main()
