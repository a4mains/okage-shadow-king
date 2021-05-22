#!/usr/bin/env python3
"""Compare inline event resource paths with original EE execution.

The isolated original disables only diagnostic logging at0x1c7910. Lookup,
copy/format helpers, callback and entry dispatch are unmodified. Whole resource
loading, filesystem behavior and other string services are outside this probe.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from prepare_assets import ELF_HASH
from verify_event import digest, write_exact

def script(writes, strings, samples=b''):
    words = [1, 4]
    for selector, offset, a in writes:
        words.extend([(selector << 20) | (a << 8) | 0x59, offset])
    words.append(0)
    return (struct.pack('<8I', 0x544d58, len(words)*4, len(samples), len(strings), 0xffffffff, 0, 1, 0)
            + struct.pack('<'+'I'*len(words), *words) + samples + strings)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native-release/okage-assets'))
    p.add_argument('--oracle', type=Path, default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference', type=Path, help='Reuse completed original cases from a previous report directory')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    elf = Path('local/extracted/SCUS_971.29')
    if digest(elf) != ELF_HASH:
        raise ValueError('Wrong retail executable')
    a.output.mkdir(parents=True, exist_ok=False)
    fixture = [
        ('map', [(0x100, 0, 0xfff)], b'vstart_t\0', b''),
        ('directory', [(0x101, 0, 0x123)], b'map/vil\0', b''),
        ('offset-and-samples', [(0x100, 7, 0), (0x101, 15, 0)], b'unused\0variant\0map/new\0', bytes(range(8))),
        ('empty', [(0x100, 0, 0), (0x101, 0, 0)], b'\0', b''),
        ('max-map', [(0x100, 0, 0)], b'a'*59+b'\0', b''),
        ('max-directory', [(0x101, 0, 0)], b'b'*54+b'\0', b''),
        ('shorter-overwrite', [(0x100, 0, 0), (0x100, 17, 0)], b'longer_map_name\0\0x\0', b''),
        ('numeric-precedence', [(0x300, 0xdeadbeef, 0)], b'\0', b''),
    ]
    cases = [(name, script(writes, table, samples), bytes(0x1000), bytes([0xa7])*0x800)
             for name, writes, table, samples in fixture]
    variables = bytearray(Path('local/native-bakery-ending-14620-release-v1.png.variables.bin').read_bytes())
    if len(variables) != 0x1000:
        raise ValueError('Native variable extent')
    variables[0x10:0x110] = bytes(0x100)
    for index, value in [(0, 0), (1, 4), (6, 1)]:
        struct.pack_into('<I', variables, 0x510+index*4, value)
    village = Path('local/xpf-decoded/FLDDATA/EVENT/EVDATA.XPF/e_vil.xmt')
    if digest(village) != 'b8031d37a50f488e030035b9b1bc4ffa57fde8a02bebab355f269263be89c7d3':
        raise ValueError('Changed owned village script')
    cases.append(('village-entry', village.read_bytes(), bytes(variables), bytes(0x800)))
    previous = json.loads((a.reference/'report.json').read_text()) if a.reference else None
    if previous and previous['elf_sha256'] != ELF_HASH:
        raise ValueError('Reference ELF differs')
    records = []
    for index, (name, code, variables, initial) in enumerate(cases):
        root = a.output/name; root.mkdir()
        paths = [root/n for n in ['script.xmt', 'variables.bin', 'initial.bin']]
        for path, data in zip(paths, [code, variables, initial]):
            write_exact(path, data)
        hashes = [digest(path) for path in paths]
        output = root/'native'
        native_cmd = [str(a.native), 'event-setup', *map(str, paths), str(output)]
        subprocess.run(native_cmd, check=True, stdout=subprocess.DEVNULL)
        original = root/'original.bin'
        if previous:
            old = previous['cases'][index]
            if old['name'] != name or old['fixture_sha256'] != hashes or old['original_exit_code'] != 0:
                raise ValueError('Changed reference fixture')
            source = a.reference/name/'original.bin'
            if digest(source) != old['result_sha256']:
                raise ValueError('Changed original result')
            write_exact(original, source.read_bytes())
            original_cmd = old['original_command']
        else:
            original_cmd = [str(a.oracle), 'event-setup-strings', str(elf), *map(str, paths), str(original)]
            with (root/'original.log').open('w') as log:
                subprocess.run(original_cmd, check=True, stdout=log, stderr=subprocess.STDOUT)
        result = Path(str(output)+'.state.bin')
        if result.read_bytes() != original.read_bytes():
            raise ValueError(name+': native/original inline field state differs')
        if name == 'village-entry' and result.read_bytes()[0x2c0:0x2cd] != b'vstart_t.xpf\0':
            raise ValueError('Actual village did not select the alternate map')
        records.append(dict(name=name, fixture_sha256=hashes, result_sha256=digest(result),
                            exact_state_bytes=2048, original_exit_code=0,
                            native_command=native_cmd, original_command=original_cmd))
    report = dict(scope=__doc__, elf_sha256=ELF_HASH, native_sha256=digest(a.native),
                  oracle_sha256=previous['oracle_sha256'] if previous else digest(a.oracle),
                  reused_reference=str(a.reference) if a.reference else None,
                  cases=records, exact_state_bytes=sum(c['exact_state_bytes'] for c in records))
    write_exact(a.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print('Inline path comparison exact:', len(records), 'cases,', report['exact_state_bytes'], 'bytes')

if __name__ == '__main__':
    main()
