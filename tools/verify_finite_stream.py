#!/usr/bin/env python3
"""Audit original EE finite-stream preparation and native dry voice output.

The original producer runs with synchronous SIF copies into fixture IOP memory.
This validates flags, alignment, EOF commands and linear playback bytes, not DMA
latency, live ring refill scheduling, shared voices, reverb or physical output.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from build_voice_reference import build, HASHES
from build_spu_reference import REVISION
from verify_event import write_exact

def digest(data):
    return hashlib.sha256(data).hexdigest()

def finite_sample(source):
    """Independent expression of EE0x173088/0x173940's linear data contract."""
    if not source or len(source) % 16 or len(source) > 512*1024:
        raise ValueError('Finite stream fixture extent')
    data = bytearray(source)
    data[1::16] = bytes(len(source)//16)
    data.extend(bytes(-len(data) % 64))
    data.extend(b'\x00\x01' + bytes(62))
    return bytes(data)

def audit_original(source, raw, commands, context, ring=4096):
    expected = bytearray(finite_sample(source))
    aligned = len(expected)-64
    # Physical SPU wrap flags occur only in source/padding, never the EOF copy.
    for end in range(ring, aligned+1, ring):
        expected[end-15] = 3
    if raw != expected:
        raise ValueError('Original producer bytes differ from recovered contract')
    if len(context) != 64:
        raise ValueError('Original context extent')
    words = struct.unpack('<16I', context)
    if words != (0, 256, 1, 0, 0, 0x40000, 0x100000, aligned, aligned,
                 0, 0, ring, aligned % ring, 0, aligned, 0):
        raise ValueError('Original EOF counters/state differ')
    expected_commands = bytearray()
    remainder = 0
    for at in range(0, len(source), 4096):
        count = min(4096, len(source)-at)
        available = remainder+count
        expected_commands.extend(struct.pack('<IIIBB2xI', 0xa1, 0, 8, 0, 0, available & ~63))
        remainder = available & 63
    if remainder:
        expected_commands.extend(struct.pack('<IIIBB2xI', 0xa1, 0, 8, 0, 0, 64))
    expected_commands.extend(struct.pack('<IIIBB2xI', 0xa1, 0, 8, 0, 1, 64))
    if commands != expected_commands:
        raise ValueError('Original stream availability/EOF commands differ')
    linear = bytearray(raw)
    for end in range(ring, aligned+1, ring):
        linear[end-15] = 0
    return bytes(linear)

def bank_payload(bank, index):
    chunks = {}
    at = 16
    while at < len(bank):
        tag, size = struct.unpack_from('<4sI', bank, at)
        chunks[tag] = at+16
        at += 16+size
    record = chunks[b'FORM']+4
    for _ in range(index):
        record += bank[record+1]
    offset, size = struct.unpack_from('<II', bank, record+16)
    rate = struct.unpack_from('<H', bank, record+10)[0]
    start = chunks[b'BODY']+offset
    return bank[start:start+size], start, rate

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bank', choices=['ALL','SET2','SET10','SET11','SET12','SET13','SET14','SET15','SET16'], action='append', help='Select reviewed banks; defaults to the earlier ALL/SET10 corpus')
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--oracle', type=Path, default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--original', type=Path, default=Path('local/finite-stream-original-v1'))
    p.add_argument('--output', type=Path, default=Path('local/finite-stream-native-v1'))
    args = p.parse_args()
    args.original.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    reference = None
    cases = []
    banks = [('ALL', [38, 35], 'a422f83c70125c775cf234fdb195a4590253bcebf87d0fcf65727eaa5d5fea1a'),
             ('SET2', [0], 'e83e0e53c7cb372d96c33a1867b2e3b54c5d30711abddc999cd483eaade3470a'),
             ('SET10', list(range(5)), '84062be8c8b4d10d036e4c4ee4897a08cf875d57abe1bbd71748258ca887200a'),
             ('SET11', [0,1], '2e5c7bd2f1083c0213723505d505c50421b4146133fe94fd6daca7b5ccf1853f'),
             ('SET12', [0], 'e4c07efc18a723862d1bba4759d966c2f2da8d341270ec38b8edd100c20c820c'),
             ('SET13', [0], '467e05cbad272ab6428b8bdf2dab44b8754beb0b99504f24b6eb4efbf6da7011'),
             ('SET14', [0], 'f269d17f17bb1e6520455cb50df6bb3fa810d168ca1702956fab4608c1d0591b'),
             ('SET15', [0], '1018e38b81009c73f4febcce0542a173292a0ace17977420ddf50b9ca09de740'),
             ('SET16', [0], '2b7eb9599b9432f548d0ebe11e79755fe88b8aaa67b6a6cd923fa06e06b988cf')]
    selected=args.bank or ['ALL','SET10']
    banks=[bank for bank in banks if bank[0] in selected]
    for name, indices, expected_hash in banks:
        path = Path('local/audio-input/SNDDATA/SE')/(name+'.XWS')
        bank = path.read_bytes()
        if digest(bank) != expected_hash:
            raise ValueError('Unexpected owned bank revision')
        for index in indices:
            key = f'{name}-{index}'
            source, offset, rate = bank_payload(bank, index)
            sample = args.original/(key+'.source.adpcm')
            write_exact(sample, source)
            # Reuse the already completed first cue38 probe, including its manifest.
            raw_path = (Path('local/finite-stream-probe-v1/cue38.original.bin') if key == 'ALL-38'
                        else args.original/(key+'.original.bin'))
            if not raw_path.exists():
                command = [str(args.oracle), 'stream-prepare', 'local/extracted/SCUS_971.29',
                           str(sample), '4096', str(raw_path)]
                subprocess.run(command, check=True)
                write_exact(Path(str(raw_path)+'.run.json'), (json.dumps({
                    'command': command, 'exit_code': 0, 'binary_sha256': digest(args.oracle.read_bytes()),
                    'source_sha256': digest(source)}, indent=2)+'\n').encode())
            linear = audit_original(source, raw_path.read_bytes(),
                Path(str(raw_path)+'.commands.bin').read_bytes(),
                Path(str(raw_path)+'.context.bin').read_bytes())
            prepared = args.output/(key+'.native.adpcm')
            subprocess.run([str(args.native), 'stream-prepare', str(path), str(index), str(prepared)], check=True)
            if prepared.read_bytes() != linear:
                raise ValueError('Native stream preparation differs: '+key)
            linear_path = args.original/(key+'.linear.adpcm')
            write_exact(linear_path, linear)
            pitch = rate*4096//48000
            volume = 60 if key == 'ALL-35' else 128
            gain = volume*127 >> 7
            frames = 400000 if name in ('SET13','SET15') else (280000 if name in ('SET10','SET11','SET12','SET14','SET16') else (100000 if index == 38 or name=='SET2' else 48000))
            ref_pcm = args.original/(key+'.reference.pcm')
            if not ref_pcm.exists():
                if reference is None:
                    reference = build()
                subprocess.run([str(reference), 'voice', str(linear_path), '32768', '32806',
                    str(pitch), str(gain), '480000', str(frames), str(ref_pcm)], check=True)
            native_pcm = args.output/(key+'.native.pcm')
            metadata = list(map(int, subprocess.check_output([str(args.native), 'stream-cue',
                str(path), str(index), '1', str(volume), str(frames), str(native_pcm)], text=True).split()))
            if metadata != [offset, len(source), rate, pitch, gain, gain, 32768, 32806, 0]:
                raise ValueError('Native cue parameters/final state differ: '+key)
            pcm = native_pcm.read_bytes()
            if len(pcm) != frames*4 or pcm != ref_pcm.read_bytes():
                raise ValueError('Native DSP differs: '+key)
            cases.append({'cue': key, 'source_sha256': digest(source), 'source_bytes': len(source),
                'prepared_bytes': len(linear), 'prepared_sha256': digest(linear),
                'original': str(raw_path), 'original_sha256': digest(raw_path.read_bytes()),
                'frames': frames, 'pcm_sha256': digest(pcm), 'bytes_and_pcm_exact': True})
            print(key+': original preparation and dry PCM exact', flush=True)
    report = {'scope': __doc__.strip(), 'native_sha256': digest(args.native.read_bytes()),
        'oracle_sha256': digest(args.oracle.read_bytes()), 'dsp_revision': REVISION,
        'dsp_source_hashes': HASHES, 'cases': cases}
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())

if __name__ == '__main__':
    main()
