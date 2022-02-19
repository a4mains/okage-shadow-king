#!/usr/bin/env python3
"""Extract authored battle data from the hash-pinned owned retail ELF.

OKBTDAT1, little endian: magic[8], version u32=1, formation slots u32=256,
stages u32=23, entry count u32, source SHA256[32]; then 256 (first,count)
u32 pairs, 23 NUL-padded 64-byte stage paths (indices 1..23), then raw
12-byte entries: signed16 unit ID, two signed8 fields, float32 X/Z.
Absent pairs are (0,0xffffffff); present ranges are contiguous in slot order.

0x15aea8/0x15b580 index pairs at 0x246520 and entries by 12 bytes. The
256-pair static region ends at the independently referenced 0x246d20 child
descriptor (0x15b558). This is a bounded asset region, NOT an original index
check. 0x149e60 explicitly clamps the separate stage index to 1..23 before
0x152248 indexes 0x1fce68. Entries are authored units/coordinates, not live
battle state; no formation-specific behavior is implemented by this importer.
"""
import argparse
import hashlib
import struct
from pathlib import Path

ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
MAGIC = b'OKBTDAT1'
FORMATION_COUNT = 256
STAGE_COUNT = 23
HEADER = struct.Struct('<8sIIII32s')

class ElfImage:
    def __init__(self, data):
        self.data = data
        if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
            raise ValueError('Expected ELF32 little-endian image')
        if struct.unpack_from('<H', data, 18)[0] != 8:
            raise ValueError('Expected MIPS ELF')
        phoff = struct.unpack_from('<I', data, 28)[0]
        entsize, count = struct.unpack_from('<HH', data, 42)
        if entsize != 32 or count == 0 or phoff + count * entsize > len(data):
            raise ValueError('Invalid ELF program headers')
        self.segments = []
        for i in range(count):
            kind, off, va, _, size, memsize, _, _ = struct.unpack_from('<8I', data, phoff+i*32)
            if kind != 1:
                continue
            if size > memsize or off+size > len(data) or va+memsize > 0x100000000:
                raise ValueError('Invalid ELF load segment')
            self.segments.append((va, off, size))

    def read(self, address, size):
        matches = [(off+address-va, size) for va, off, extent in self.segments
                   if va <= address and size >= 0 and address+size <= va+extent]
        if len(matches) != 1:
            raise ValueError(f'Unmapped or ambiguous ELF range {address:#x}+{size:#x}')
        off, size = matches[0]
        return self.data[off:off+size]

def extract_tables(image, source_digest):
    """Structural extraction; public import_elf also enforces the retail hash."""
    if len(source_digest) != 32:
        raise ValueError('Invalid source digest')
    # Independently referenced descriptor at the end of the static pair region.
    if image.read(0x246d20, 16) != struct.pack('<4I', 0x15b580, 0x15b9a0, 0x15c138, 0x15c1b0):
        raise ValueError('Formation region end descriptor differs')
    directory = bytearray()
    entries = bytearray()
    for slot in range(FORMATION_COUNT):
        pointer, count = struct.unpack('<Ii', image.read(0x246520+slot*8, 8))
        if count == -1 and pointer == 0:
            directory += struct.pack('<II', 0, 0xffffffff)
            continue
        # 10 child pointers fit the original 0x64-byte container at +0x3c.
        # All authored lists lie between descriptor 0x244be0 and the pair table.
        if not 1 <= count <= 10 or pointer % 4 or not 0x244bf0 <= pointer < 0x246520 or pointer+12*count > 0x246520:
            raise ValueError(f'Invalid formation slot {slot}')
        directory += struct.pack('<II', len(entries)//12, count)
        entries += image.read(pointer, count*12)
    stages = bytearray()
    prefix = b'btldata/beta2/field/'
    for stage in range(1, STAGE_COUNT+1):
        pointer, = struct.unpack('<I', image.read(0x1fce68+stage*4, 4))
        name = bytearray()
        for i in range(64-len(prefix)):
            c = image.read(pointer+i, 1)[0]
            if c == 0:
                break
            if not (ord('a') <= c <= ord('z') or ord('0') <= c <= ord('9') or c in b'_.'):
                raise ValueError('Invalid stage resource name')
            name.append(c)
        else:
            raise ValueError('Unterminated stage resource name')
        if len(name) <= 4 or not name.endswith(b'.xpf') or b'..' in name:
            raise ValueError('Invalid stage resource path')
        path = prefix + name
        stages += path + bytes(64-len(path))
    return HEADER.pack(MAGIC, 1, FORMATION_COUNT, STAGE_COUNT, len(entries)//12, source_digest) + directory + stages + entries

def import_elf(data):
    digest = hashlib.sha256(data).digest()
    if digest.hex() != ELF_SHA256:
        raise ValueError('Owned ELF SHA256 does not match supported retail revision')
    return extract_tables(ElfImage(data), digest)

def write_output(path, data):
    if path.is_symlink():
        raise ValueError('Refusing symlink output')
    if path.exists():
        if path.read_bytes() != data:
            raise ValueError('Refusing to replace a different battle data artifact')
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as stream:
        stream.write(data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    data = import_elf(args.elf.read_bytes())
    write_output(args.output, data)
    print(f'{len(data)} bytes; SHA256 {hashlib.sha256(data).hexdigest()}')

if __name__ == '__main__':
    main()
