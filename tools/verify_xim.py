#!/usr/bin/env python3
"""Compare native XIM metadata to original retail accessor routines.

Also validates the two observed 16-bit-palette textures against the retail
palette conversion routine. This does not replace in-game renderer validation.
"""
import collections
import hashlib
import json
import struct
import subprocess
from pathlib import Path
import unicorn
from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS64, UC_MODE_LITTLE_ENDIAN
from unicorn.mips_const import *
from inspect_disc import sha256
from prepare_assets import ELF_HASH

def main():
    elf = Path('local/extracted/SCUS_971.29')
    if sha256(elf) != ELF_HASH:
        raise ValueError('Wrong retail executable')
    u = Uc(UC_ARCH_MIPS, UC_MODE_MIPS64 | UC_MODE_LITTLE_ENDIAN)
    u.ctl_set_cpu_model(UC_CPU_MIPS64_MIPS64R2_GENERIC)
    u.mem_map(0x100000, 0x200000)
    u.mem_write(0x100000, elf.read_bytes()[4096:4096+1369296])
    source, output, stop = 0x1000000, 0x3000000, 0x2ff000
    u.mem_map(source, 0x1000000)
    u.mem_map(output, 0x1000000)

    def call(address, *args):
        for reg, value in zip((UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3), args):
            u.reg_write(reg, value)
        u.reg_write(UC_MIPS_REG_SP, 0x3ffff00)
        u.reg_write(UC_MIPS_REG_RA, stop)
        try:
            u.emu_start(address, stop, count=100000)
        except Exception as exc:
            raise RuntimeError(f'Oracle exception at PC {u.reg_read(UC_MIPS_REG_PC):08x}') from exc
        if u.reg_read(UC_MIPS_REG_PC) != stop:
            raise ValueError('Retail accessor did not return')
        return u.reg_read(UC_MIPS_REG_V0)

    binary = Path('build/native/okage-assets').resolve()
    run = subprocess.run([str(binary), 'texture-check', 'local/xpf-decoded'],
                         check=True, capture_output=True, text=True)
    formats, results, palette_checks = collections.Counter(), [], 0
    for line in run.stdout.splitlines():
        path, width, height, psm, cpsm = line.split('\t')
        width, height, psm, cpsm = map(int, (width, height, psm, cpsm))
        data = Path(path).read_bytes()
        u.mem_write(source, data)
        if call(0x18f4d0, source) != psm or call(0x18f4e0, source) != cpsm:
            raise ValueError(f'Format mismatch: {path}')
        dims = call(0x18f5a8, source)
        if (dims & 0xffffffff, dims >> 32) != (height, width):
            raise ValueError(f'Dimension mismatch: {path}')
        if cpsm == 2:
            count = struct.unpack_from('<I', data, 28)[0]
            u.mem_write(0x2138c4, struct.pack('<I', 0))  # Original default: black transparent.
            call(0x1cda18, count, source + 32, output)
            palette = bytes(u.mem_read(output, count*4))
            offset = 32 + struct.unpack_from('<I', data, 16)[0]
            expected = bytearray()
            for index in data[offset:]:
                color = palette[index*4:index*4+4]
                expected.extend((*color[:3], min(255, color[3]*2)))
            dest = Path('local/xim-palette-oracle') / (hashlib.sha256(data).hexdigest() + '.pam')
            subprocess.run([str(binary), 'texture', path, str(dest)], check=True, capture_output=True)
            actual = dest.read_bytes().split(b'ENDHDR\n', 1)[1]
            if actual != expected:
                raise ValueError(f'Palette conversion mismatch: {path}')
            palette_checks += 1
        formats[f'psm={psm},cpsm={cpsm}'] += 1
        results.append({'path': path, 'width': width, 'height': height, 'psm': psm, 'cpsm': cpsm})
    report = {'elf_sha256': ELF_HASH, 'native_binary_sha256': sha256(binary),
              'oracle': 'Unicorn ' + unicorn.__version__ + ' MIPS64R2_GENERIC', 'texture_count': len(results),
              'formats': dict(formats), 'palette16_pixel_checks': palette_checks,
              'limits': 'All dimensions/formats compared to original accessors; image preview color checks are not in-game GS fidelity tests.',
              'results': results}
    Path('local/xim-verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(results)} texture format/dimension checks; {palette_checks} palette pixel checks')

if __name__ == '__main__':
    main()
