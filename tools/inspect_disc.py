#!/usr/bin/env python3
"""Read-only ISO9660/PS2 ELF reconnaissance. No external dependencies.

Accepts a cooked 2048-byte ISO or a single data track at LBA 0 in raw
MODE1/2352 or MODE2/2352 format. Not a general CD image converter.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

def sha256(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()

class Disc:
    def __init__(self, path):
        self.path = Path(path)
        self.f = self.path.open('rb')
        self.size = self.path.stat().st_size
        for stride, offset in [(2048, 0), (2352, 24), (2352, 16)]:
            self.f.seek(16 * stride + offset)
            if self.f.read(7) == b'\x01CD001\x01':
                self.stride, self.offset = stride, offset
                break
        else:
            self.f.close()
            raise ValueError('No ISO9660 primary descriptor at LBA 16')
        if self.size % self.stride:
            self.f.close()
            raise ValueError('Truncated sector at end of disc')

    def close(self):
        self.f.close()

    def sector(self, lba):
        if lba < 0 or (lba + 1) * self.stride > self.size:
            raise ValueError(f'Sector outside disc: {lba}')
        self.f.seek(lba * self.stride)
        raw = self.f.read(self.stride)
        if self.stride == 2352:
            if raw[:12] != b'\x00' + b'\xff' * 10 + b'\x00':
                raise ValueError(f'Invalid sector sync: {lba}')
            if self.offset == 24:
                if raw[15] != 2 or raw[16:20] != raw[20:24] or raw[18] & 0x20:
                    raise ValueError(f'Expected Mode 2 Form 1 sector: {lba}')
            elif raw[15] != 1:
                raise ValueError(f'Expected Mode 1 sector: {lba}')
        return raw[self.offset:self.offset + 2048]

    def read(self, lba, size):
        if size < 0 or lba < 0 or (lba + (size + 2047) // 2048) * self.stride > self.size:
            raise ValueError('Extent outside disc')
        return b''.join(self.sector(lba + i) for i in range((size + 2047) // 2048))[:size]

    @staticmethod
    def record(data):
        if len(data) < 34 or data[0] != len(data) or 33 + data[32] > len(data):
            raise ValueError('Malformed ISO directory record')
        lba, size = struct.unpack_from('<I', data, 2)[0], struct.unpack_from('<I', data, 10)[0]
        if lba != struct.unpack_from('>I', data, 6)[0] or size != struct.unpack_from('>I', data, 14)[0]:
            raise ValueError('Inconsistent ISO endian fields')
        if data[1] or data[25] & 0x80 or data[26] or data[27]:
            raise ValueError('Extended attributes, multi-extent, or interleaved files unsupported')
        return lba, size, data[25], data[33:33 + data[32]]

    def inventory(self):
        pvd = self.sector(16)
        if struct.unpack_from('<H', pvd, 128)[0] != 2048:
            raise ValueError('Unsupported logical block size')
        self.volume = pvd[40:72].decode('ascii').strip()
        root = self.record(pvd[156:156 + pvd[156]])
        files, visited = [], set()

        def walk(lba, size, prefix):
            if lba in visited:
                raise ValueError('Repeated directory extent')
            visited.add(lba)
            data, pos = self.read(lba, size), 0
            while pos < len(data):
                length = data[pos]
                if length == 0:
                    pos = (pos // 2048 + 1) * 2048
                    continue
                if pos % 2048 + length > 2048:
                    raise ValueError('Directory record crosses sector')
                child, n, flags, name = self.record(data[pos:pos + length])
                pos += length
                if name in (b'\0', b'\1'):
                    continue
                name = name.decode('ascii').split(';')[0]
                if not name or name in ('.', '..') or '/' in name or '\\' in name:
                    raise ValueError('Unsafe ISO filename')
                path = prefix + name
                if flags & 2:
                    walk(child, n, path + '/')
                else:
                    if (child + (n + 2047) // 2048) * self.stride > self.size:
                        raise ValueError('File extent outside disc')
                    files.append({'path': path, 'lba': child, 'size': n})

        walk(root[0], root[1], '')
        return sorted(files, key=lambda x: x['path'])

def inspect_elf(data):
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Expected ELF32 little endian executable')
    fields = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    kind, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phnum, shsize, shnum, shstr = fields
    if machine != 8 or version != 1 or ehsize != 52:
        raise ValueError('Expected MIPS ELF32')
    if (phnum and phsize < 32) or (shnum and shsize < 40):
        raise ValueError('Invalid ELF table stride')

    def bounded(offset, size):
        if offset < 0 or offset + size > len(data):
            raise ValueError('ELF range outside file')
        return data[offset:offset + size]

    bounded(phoff, phsize * phnum)
    bounded(shoff, shsize * shnum)
    sections = [struct.unpack_from('<10I', data, shoff + i * shsize) for i in range(shnum)]
    if shnum and shstr >= shnum:
        raise ValueError('Invalid ELF section-name table')
    names = bounded(sections[shstr][4], sections[shstr][5]) if shnum and shstr else b''

    def string(table, offset):
        return table[offset:].split(b'\0', 1)[0].decode('ascii', errors='replace')

    sec = []
    for s in sections:
        if s[1] != 8:  # SHT_NOBITS has no file payload.
            bounded(s[4], s[5])
        sec.append({'name': string(names, s[0]), 'type': s[1], 'address': f'0x{s[3]:08x}',
                    'offset': s[4], 'size': s[5], 'flags': s[2]})
    segments = []
    for i in range(phnum):
        p = struct.unpack_from('<8I', data, phoff + i * phsize)
        bounded(p[1], p[4])
        segments.append({'type': p[0], 'offset': p[1], 'vaddr': f'0x{p[2]:08x}',
                         'filesz': p[4], 'memsz': p[5], 'flags': p[6]})
    symcount = sum(s[5] // s[9] for s in sections if s[1] in (2, 11) and s[9])
    return {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'type': kind, 'machine': machine, 'entry': f'0x{entry:08x}', 'flags': f'0x{flags:08x}',
            'symbol_entries': symcount, 'sections': sec, 'segments': segments}

def inspect(path, extract=None):
    disc = Disc(path)
    try:
        files = disc.inventory()
        boot = next((f for f in files if f['path'].upper() == 'SYSTEM.CNF'), None)
        if boot is None:
            raise ValueError('Missing SYSTEM.CNF')
        cnf = disc.read(boot['lba'], boot['size']).decode('ascii')
        match = re.search(r'BOOT2\s*=\s*cdrom0:[\\/]*([^;\r\n]+)', cnf, re.I)
        if not match:
            raise ValueError('Missing BOOT2 path')
        executable = match[1].replace('\\', '/')
        target = next(f for f in files if f['path'].upper() == executable.upper())
        elf = inspect_elf(disc.read(target['lba'], target['size']))
        extracted = []
        if extract:
            out = Path(extract)
            selected = [f for f in files if f == target or f == boot
                        or f['path'].upper() == 'IOPCONF.CNF' or f['path'].upper().startswith('MODULES/')]
            for item in selected:
                dest = out / item['path']
                dest.parent.mkdir(parents=True, exist_ok=True)
                payload = disc.read(item['lba'], item['size'])
                if dest.exists():
                    if dest.read_bytes() != payload:
                        raise ValueError(f'Refusing to overwrite different file: {dest}')
                else:
                    with dest.open('xb') as f:
                        f.write(payload)
                extracted.append(item['path'])
        return {'image': Path(path).name, 'image_size': disc.size, 'image_sha256': sha256(path),
                'sector_size': disc.stride, 'user_data_offset': disc.offset, 'volume': disc.volume,
                'system_cnf': cnf, 'boot_executable': executable, 'elf': elf,
                'files': files, 'extracted': extracted,
                'limits': 'No EDC/ECC verification, external dump verification, asset decoding, or execution performed.'}
    finally:
        disc.close()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('image', type=Path)
    p.add_argument('--extract', type=Path, help='Write boot ELF, boot configs and MODULES/ locally')
    p.add_argument('--output', type=Path, help='Write JSON metadata')
    args = p.parse_args()
    report = inspect(args.image, args.extract)
    encoded = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded)
    else:
        print(encoded, end='')

if __name__ == '__main__':
    main()
