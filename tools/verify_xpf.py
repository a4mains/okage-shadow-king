#!/usr/bin/env python3
"""Local differential test: native XPF decoder vs original retail MIPS routine.

Unicorn is a research-only dependency, never part of the native library/app.
The full corpus is the default. Outputs and detailed reports remain in local/.
"""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

from inspect_disc import Disc, sha256
from prepare_assets import ELF_HASH, HASH

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--limit-archives', type=int, default=0)
    p.add_argument('--report', type=Path, default=Path('local/xpf-verification.json'))
    args = p.parse_args()
    from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS64, UC_MODE_LITTLE_ENDIAN
    from unicorn.mips_const import UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_RA, UC_MIPS_REG_PC, UC_MIPS_REG_V0, UC_MIPS_REG_T2
    import unicorn
    image = Path('local/disc/Okage - Shadow King (USA).bin')
    elf = Path('local/extracted/SCUS_971.29')
    if sha256(image) != HASH or sha256(elf) != ELF_HASH:
        raise ValueError('Input hash differs from the reviewed code revision')
    native = Path('build/native/okage-assets').resolve()
    u = Uc(UC_ARCH_MIPS, UC_MODE_MIPS64 | UC_MODE_LITTLE_ENDIAN)
    u.mem_map(0x100000, 0x200000)
    u.mem_write(0x100000, elf.read_bytes()[4096:4096 + 1369296])
    src, dst, stop = 0x1000000, 0x3000000, 0x2ff000
    u.mem_map(src, 0x1000000)
    u.mem_map(dst, 0x1000000)
    results, archive_count, total_bytes = [], 0, 0
    disc = Disc(image)
    try:
        archives = [f for f in disc.inventory() if f['path'].endswith('.XPF')]
        if args.limit_archives:
            archives = archives[:args.limit_archives]
        for file in archives:
            data = disc.read(file['lba'], file['size'])
            packed_path = Path('local/xpf-packed') / file['path']
            packed_path.parent.mkdir(parents=True, exist_ok=True)
            if not packed_path.exists():
                packed_path.write_bytes(data)
            elif packed_path.read_bytes() != data:
                raise ValueError(f'Existing archive differs: {packed_path}')
            out = Path('local/xpf-decoded') / file['path']
            run = subprocess.run([str(native), 'extract', str(packed_path), str(out)],
                                 check=True, capture_output=True, text=True)
            rows = [line.split('\t') for line in run.stdout.splitlines()]
            count = struct.unpack_from('<I', data, 8)[0]
            if len(rows) != count:
                raise ValueError('Native directory count mismatch')
            for name, offset, packed_size, decoded_size, consumed in rows:
                offset, packed_size, decoded_size, consumed = map(int, (offset, packed_size, decoded_size, consumed))
                packed = data[offset:offset + packed_size]
                if packed_size >= 0x1000000 or decoded_size + 32 >= 0x1000000:
                    raise ValueError('Member too large for oracle buffers')
                u.mem_write(src, packed + bytes(16))
                u.mem_write(dst, bytes([0xa5]) * (decoded_size + 32))
                u.reg_write(UC_MIPS_REG_A0, dst)
                u.reg_write(UC_MIPS_REG_A1, src)
                u.reg_write(UC_MIPS_REG_RA, stop)
                u.emu_start(0x1bd000, stop, count=max(10000, decoded_size * 100))
                if u.reg_read(UC_MIPS_REG_PC) != stop:
                    raise ValueError('Retail routine did not return within instruction budget')
                expected = bytes(u.mem_read(dst, decoded_size))
                actual = (out / name).read_bytes()
                if actual != expected:
                    raise ValueError(f'Output mismatch: {file["path"]}/{name}')
                if u.reg_read(UC_MIPS_REG_V0) != decoded_size or u.reg_read(UC_MIPS_REG_T2) - dst != decoded_size:
                    raise ValueError('Retail return value or written size mismatch')
                if u.reg_read(UC_MIPS_REG_A1) - src != consumed:
                    raise ValueError('Native/retail consumed byte counts differ')
                if bytes(u.mem_read(dst + decoded_size, 32)) != bytes([0xa5]) * 32:
                    raise ValueError('Retail routine wrote beyond declared output size')
                results.append({'archive': file['path'], 'member': name, 'size': decoded_size,
                                'packed_size': packed_size, 'consumed': consumed,
                                'sha256': hashlib.sha256(actual).hexdigest()})
                total_bytes += decoded_size
            archive_count += 1
            if archive_count % 25 == 0:
                print(f'Validated {archive_count}/{len(archives)} archives, {len(results)} members', flush=True)
    finally:
        disc.close()
    report = {'elf_sha256': ELF_HASH, 'image_sha256': HASH, 'oracle': 'Unicorn ' + unicorn.__version__,
              'routine': '0x001bd000', 'native_binary_sha256': sha256(native),
              'archives': archive_count, 'members': len(results), 'decoded_bytes': total_bytes,
              'all_corpus': not args.limit_archives, 'results': results}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {archive_count} archives / {len(results)} members / {total_bytes} bytes', flush=True)

if __name__ == '__main__':
    main()
