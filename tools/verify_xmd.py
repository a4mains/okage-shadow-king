#!/usr/bin/env python3
"""Validate native model structure against retail metadata/command scanning.

Research-only original-code execution. Does not verify VU skinning, polygon
rasterization, material/GS semantics, bone matrices or animation playback.
"""
import collections
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
    binary = Path('build/native/okage-assets').resolve()
    corpus = Path('local/xpf-decoded')
    files = sorted(p for p in corpus.rglob('*') if p.suffix.lower() == '.xmd')
    if not files:
        raise ValueError('Missing XMD corpus')
    run = subprocess.run([str(binary), 'model-check', str(corpus)],
                         check=True, capture_output=True, text=True)
    u = Uc(UC_ARCH_MIPS, UC_MODE_MIPS64 | UC_MODE_LITTLE_ENDIAN)
    u.ctl_set_cpu_model(UC_CPU_MIPS64_MIPS64R2_GENERIC)
    u.mem_map(0x100000, 0x300000)
    u.mem_write(0x100000, elf.read_bytes()[4096:4096+1369296])
    source, stop, stack = 0x1000000, 0x2ff000, 0x3fff00
    u.mem_map(source, 0x1000000)

    def call(address, *args):
        for reg, value in zip((UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3), args):
            u.reg_write(reg, value)
        u.reg_write(UC_MIPS_REG_SP, stack)
        u.reg_write(UC_MIPS_REG_RA, stop)
        try:
            u.emu_start(address, stop, count=1000000)
        except Exception as exc:
            raise RuntimeError(f'Oracle exception at PC {u.reg_read(UC_MIPS_REG_PC):08x}') from exc
        if u.reg_read(UC_MIPS_REG_PC) != stop:
            raise ValueError('Retail helper did not return')
        return u.reg_read(UC_MIPS_REG_V0)

    totals, results, observed = collections.Counter(), [], []
    for line in run.stdout.splitlines():
        parts = line.split('\t')
        kind = parts.pop(0)
        if kind == 'file':
            path, texture_count, model_count = parts
            data = Path(path).read_bytes()
            if len(data) > 0x1000000:
                raise ValueError('Model exceeds oracle memory')
            u.mem_write(source, data)
            root = call(0x182de8, source)-source
            if struct.unpack_from('<I', data, root)[0] != int(model_count):
                raise ValueError(f'Model count mismatch: {path}')
            if call(0x186950, source) != int(texture_count):
                raise ValueError(f'Texture count mismatch: {path}')
            observed.append(Path(path))
            result = {'path': path, 'sha256': sha256(Path(path)), 'blocks': []}
            results.append(result)
            totals['files'] += 1
        elif kind == 'model':
            offset, end, bones, hierarchy, names, blocks = map(int, parts)
            totals.update(models=1, bones=bones, hierarchy_nodes=hierarchy, names=names)
            if offset + struct.unpack_from('<I', data, offset+4)[0] != end:
                raise ValueError(f'Model record extent mismatch: {path}')
        elif kind == 'block':
            start, end, draw, texture, material, pos, normal, color, uv, commands, triangles, quads = map(int, parts)
            u.mem_write(0x330188, b'\0'*32)
            boundary = call(0x179b10, source+start+16)-source
            extents = struct.unpack('<4I', bytes(u.mem_read(0x330188, 16)))
            tables = struct.unpack('<2I', bytes(u.mem_read(0x3301a0, 8)))
            expected_tables = (source+material+4 if material else 0, source+texture+4 if texture else 0)
            if (boundary, extents, tables) != (draw, (pos, normal, color, uv), expected_tables):
                raise ValueError(f'Retail attribute scan mismatch: {path} block {start}')
            at, count, tris, qs = start+16, 0, 0, 0
            while True:
                opcode = data[at+3]
                next_at = call(0x179af8, source+at)-source
                if next_at <= at or next_at > end:
                    raise ValueError('Retail command extent out of native block bounds')
                count += 1
                if opcode == 11:
                    tris += (next_at-at-8)//24
                elif opcode == 12:
                    qs += (next_at-at-8)//32
                at = next_at
                if opcode == 1:
                    break
            if (at, count, tris, qs) != (end, commands, triangles, quads):
                raise ValueError(f'Command traversal mismatch: {path} block {start}')
            totals.update(blocks=1, commands=count, triangles=tris, quads=qs)
            result['blocks'].append({'offset': start, 'end': end, 'draw_offset': draw,
                                     'attribute_extents': list(extents), 'commands': count})
        else:
            raise ValueError('Unexpected native report record')
    if observed != files:
        raise ValueError('Native report does not cover the exact corpus')
    report = {'elf_sha256': ELF_HASH, 'native_binary_sha256': sha256(binary),
              'oracle': 'Unicorn ' + unicorn.__version__ + ' MIPS64R2_GENERIC',
              'totals': dict(totals),
              'limits': __doc__.strip(), 'results': results}
    Path('local/xmd-verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print('PASS:', json.dumps(dict(totals)))

if __name__ == '__main__':
    main()
