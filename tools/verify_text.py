#!/usr/bin/env python3
"""Recover local PME inputs and compare selection/control semantics with retail EE code.

Uses an existing opening600 RAM snapshot for the research-only selector oracle.
Native code reads only the disc-derived PME and control byte strings.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from inspect_disc import Disc
from verify_event import write_exact

def fragments(data):
    result = []
    at = 0
    while at < len(data):
        if len(data) - at < 4:
            raise ValueError('Truncated fragment length')
        size, = struct.unpack_from('<I', data, at)
        at += 4
        if size > len(data) - at:
            raise ValueError('Truncated fragment')
        result.append(data[at:at+size])
        at += size
    return result

def trace_count(data):
    at = count = 0
    while at < len(data):
        if len(data) - at < 56:
            raise ValueError('Truncated text control record')
        at += 56
        for _ in range(5):
            if len(data) - at < 4:
                raise ValueError('Truncated text metadata size')
            size, = struct.unpack_from('<I', data, at)
            at += 4
            if size > len(data) - at:
                raise ValueError('Truncated text metadata')
            at += size
        count += 1
    return count

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--output', type=Path, default=Path('local/text-validation-v4'))
    p.add_argument('--ram', type=Path, default=Path('local/research/opening600-ram.bin'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {'scope': 'PME import, original message selection and text control semantics; no frame timing or font rendering',
              'native_sha256': hashlib.sha256(args.native.read_bytes()).hexdigest(),
              'banks': [], 'messages': [], 'controls': []}
    disc = Disc('local/disc/Okage - Shadow King (USA).bin')
    try:
        for entry in json.loads(Path('docs/disc-report.json').read_text())['files']:
            if not entry['path'].endswith('.PME'):
                continue
            data = disc.read(entry['lba'], entry['size'])
            dest = Path('local/text-input')/entry['path']
            dest.parent.mkdir(parents=True, exist_ok=True)
            write_exact(dest, data)
            info = subprocess.check_output([str(args.native), 'pme-info', str(dest)], text=True).strip()
            report['banks'].append({'path': entry['path'], 'sha256': hashlib.sha256(data).hexdigest(), 'native_info': info})
    finally:
        disc.close()
    ram = args.ram.read_bytes()
    if len(ram) != 64*1024*1024:
        raise ValueError('Expected captured 64 MiB RAM image')
    ee = args.output/'opening600-ee.bin'
    write_exact(ee, ram[:32*1024*1024])
    report['ram_sha256'] = hashlib.sha256(ram).hexdigest()
    oracle = ['build/play-research/okage-oracle/okage-pose-oracle']
    elf = 'local/extracted/SCUS_971.29'
    message_inputs = []
    report['fallback_limit'] = 'Missing-ID original path reaches a logging syscall unsupported by the isolated oracle (PC 0x1e4448). Fallback has static-analysis and native unit coverage only.'
    for ident in (10100, 10101, 10102):
        for kind, command in (('native', [str(args.native), 'pme-indices', 'local/text-input/FLDDATA/MES/FIELD.PME']),
                              ('retail', oracle+['pme-indices', elf, str(ee)])):
            subprocess.run(command+[str(ident), str(args.output/(str(ident)+'.indices.'+kind+'.bin'))], check=True)
        indices = (args.output/(str(ident)+'.indices.native.bin')).read_bytes()
        if indices != (args.output/(str(ident)+'.indices.retail.bin')).read_bytes():
            raise RuntimeError(f'Message {ident}: original/native selection mismatch')
        entry = {'id': ident, 'fragments': len(indices)//4, 'selection_exact': True,
                 'indices_sha256': hashlib.sha256(indices).hexdigest()}
        report['messages'].append(entry)
        if ident not in (10100, 10101):
            continue
        native, retail = (args.output/(str(ident)+suffix) for suffix in ('.native.bin', '.retail.bin'))
        subprocess.run([str(args.native), 'pme-message', 'local/text-input/FLDDATA/MES/FIELD.PME', str(ident), str(native)], check=True)
        subprocess.run(oracle+['pme-message', elf, str(ee), str(ident), str(retail)], check=True)
        data = native.read_bytes()
        if ident == 10100 and data != retail.read_bytes():
            raise RuntimeError(f'Message {ident}: original/native mismatch')
        pieces = fragments(data)
        entry['static_strings_sha256'] = hashlib.sha256(data).hexdigest()
        entry['live_strings_exact'] = data == retail.read_bytes()
        if ident == 10101:
            differences = [(a, b) for a, b in zip(pieces, fragments(retail.read_bytes())) if a != b]
            if differences != [(b'\\r[############]lukarucaruka\r\n', b'\\r[############]Ari')]:
                raise RuntimeError('Unexpected live player-name substitution')
            entry['live_difference'] = 'One tagged player-name string is replaced at runtime; native name binding remains unimplemented.'
        message_inputs.append((str(ident), b''.join(pieces)))
    synthetic = (b'\\m[Voice]\\y[WINTYPE(2)]\\o[BC]\\z[replace]\\d[caption]\\n'
                 b'\\c[4294967295]\\e\\c[0]\\f[3]\\x[7]\\w[160]\\v\\l'
                 b'\\m[]\\y[]\\d[\x81\x5dX]\\t\\b\r\n'
                 b'\\c[  100]\\e\\c[0]\\w[-1]')
    for name, source in message_inputs+[('synthetic', synthetic)]:
        input_path = args.output/(name+'.controls.input')
        native, retail = (args.output/(name+suffix) for suffix in ('.controls.native.bin', '.controls.retail.bin'))
        write_exact(input_path, source)
        subprocess.run([str(args.native), 'text-controls', str(input_path), str(native)], check=True)
        subprocess.run(oracle+['text-controls', elf, str(input_path), str(retail)], check=True)
        data = native.read_bytes()
        if data != retail.read_bytes():
            raise RuntimeError(name+': original/native control mismatch')
        report['controls'].append({'name': name, 'calls': trace_count(data), 'exact': True,
                                   'input_sha256': hashlib.sha256(source).hexdigest(),
                                   'output_sha256': hashlib.sha256(data).hexdigest()})
    integration = args.output/'opening-text-wait.native.bin'
    subprocess.run([str(args.native), 'text-wait', 'local/text-input/FLDDATA/MES/FIELD.PME', '10100', '2', str(integration)], check=True)
    rows = list(struct.iter_unpack('<7I', integration.read_bytes()))
    if rows[-1][2:6] != (2, 0, 0, 2) or [r[3] for r in rows[-3:]] != [1, 0xffffffff, 0]:
        raise RuntimeError('Native text did not drive the recovered event wait')
    report['integration'] = {'scope': 'Unpaced native PME -> text commands -> event wait; pauses automatically resumed, layers assumed ready',
                             'steps': len(rows), 'signals_at_release': 2,
                             'output_sha256': hashlib.sha256(integration.read_bytes()).hexdigest(),
                             'retail_frame_timing_verified': False}
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
