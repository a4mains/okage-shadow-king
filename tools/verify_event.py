#!/usr/bin/env python3
"""Compare native field-event setup with original EE execution and boot capture."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct
import subprocess

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def write_exact(path, data):
    if path.is_symlink():
        raise RuntimeError("Refusing symlink output: " + str(path))
    if path.exists():
        if path.read_bytes() != data:
            raise RuntimeError("Refusing changed output: " + str(path))
    else:
        path.write_bytes(data)

def synthetic():
    variables = bytearray(0x1000)
    variables[0x10] = 0x81
    variables[0x110] = 0x40
    variables[0x210] = 0x80
    variables[0x410] = 0xff
    struct.pack_into('<I', variables, 0x510, 0x80000000)
    words = [1, 4]
    cases = 0
    for selector in (0, 1, 7, 0x10001, 0x20000, 0x30000, 0x40000):
        for comparison in (0, 1, 2, 3, 4, 5, 6, 7, 4095):
            for expected in (0x80000000, 0xffffffff, 0, 1, 0x7fffffff):
                words.extend([(18 << 20) | (comparison << 8) | 11, selector,
                              expected, (cases << 20) | (0x55 << 8) | 0x50])
                cases += 1
    for index in (0, 1, 31, 255):
        words.extend([((0x200 | index) << 20) | 0x59, 0xdead0000 | index])
    for index in (0, 1, 31):
        words.append((index << 20) | (0xfff << 8) | 0x59)
    words.append(0)
    script = struct.pack('<8I', 0x544d58, len(words)*4, 0, 0, 0xffffffff, 0, 1, 0)
    script += struct.pack('<' + 'I'*len(words), *words)
    return script, variables, bytes([0xa7])*0x800, cases

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--oracle', type=Path, default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    parser.add_argument('--capture', type=Path, default=Path('local/reference-opening-events-400-v4'))
    parser.add_argument('--output', type=Path, default=Path('local/event-validation'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    elf = Path('local/extracted/SCUS_971.29')
    asset = Path('local/xpf-decoded/FLDDATA/EVENT/EVDATA.XPF/e_home1c.xmt')
    script, variables, state = (args.capture / name for name in ('0.xmt', '0.variables.bin', '0.field.bin'))
    if asset.read_bytes() != script.read_bytes():
        raise RuntimeError('Captured script is not the supplied HOME1C event asset')
    rows = list(csv.DictReader((args.capture/'instructions.tsv').open(), delimiter='\t'))
    rows = [row for row in rows if row['script'] == '0']
    if not rows or int(rows[-1]['word'], 16) & 255:
        raise RuntimeError('Capture lacks a completed event entry')
    original_end = args.capture / ('0.end-' + rows[-1]['observation'] + '.field.bin')
    expected_pcs = struct.pack('<' + 'I'*len(rows), *(int(row['offset'], 16) for row in rows))
    report = {'scope': 'HOME1C entry-zero setup, not opening event playback',
              'elf_sha256': digest(elf), 'native_sha256': digest(args.native),
              'oracle_sha256': digest(args.oracle), 'cases': []}

    def compare(name, script, variables, state, captured_state=None, pcs=None):
        prefix = args.output / name
        original = args.output / (name + '.retail.bin')
        subprocess.run([str(args.native), 'event-setup', str(script), str(variables),
                        str(state), str(prefix)], check=True)
        subprocess.run([str(args.oracle), 'event-setup', str(elf), str(script),
                        str(variables), str(state), str(original)], check=True)
        result = Path(str(prefix) + '.state.bin')
        if result.read_bytes() != original.read_bytes():
            raise RuntimeError(name + ': original/native field state differs')
        if captured_state and result.read_bytes() != captured_state.read_bytes():
            raise RuntimeError(name + ': full-game/native field state differs')
        if pcs is not None and Path(str(prefix) + '.pcs.bin').read_bytes() != pcs:
            raise RuntimeError(name + ': full-game/native instruction path differs')
        report['cases'].append({'name': name, 'script_sha256': digest(script),
                                'variables_sha256': digest(variables),
                                'initial_state_sha256': digest(state),
                                'result_sha256': digest(result), 'exact_state_bytes': result.stat().st_size,
                                'full_game_instruction_path': pcs is not None})

    compare('opening', script, variables, state, original_end, expected_pcs)
    data, values, initial, conditions = synthetic()
    paths = [args.output/name for name in ('synthetic.xmt', 'synthetic.variables.bin', 'synthetic.field.bin')]
    for path, payload in zip(paths, (data, values, initial)):
        write_exact(path, payload)
    compare('synthetic', *paths)
    report['opening_instructions'] = len(rows)
    report['synthetic_conditions'] = conditions
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
