#!/usr/bin/env python3
"""Opening-bank native sample decode compared with the pinned research SPU."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import wave
import zipfile
import zlib
from verify_event import write_exact
from build_spu_reference import build, REVISION, SOURCE_SHA256

def sha(data):
    return hashlib.sha256(data).hexdigest()

def state_spuram(path):
    with zipfile.ZipFile(path) as archive:
        info = archive.getinfo('iop_spuram')
    if info.file_size != 2*1024*1024 or info.compress_type != 93:
        raise RuntimeError('Unexpected reference SPU snapshot extent/compression')
    with path.open('rb') as stream:
        stream.seek(info.header_offset)
        header = stream.read(30)
        if header[:4] != b'PK\x03\x04':
            raise RuntimeError('Invalid ZIP local member header')
        name_size, extra_size = struct.unpack_from('<HH', header, 26)
        stream.seek(name_size + extra_size, 1)
        compressed = stream.read(info.compress_size)
    decoder = shutil.which('zstd')
    if not decoder:
        raise RuntimeError('zstd is required to read the reference snapshot')
    data = subprocess.run([decoder, '-d', '-c'], input=compressed, stdout=subprocess.PIPE, check=True).stdout
    if len(data) != info.file_size or zlib.crc32(data) != info.CRC:
        raise RuntimeError('Reference SPU snapshot checksum mismatch')
    return data

def opening_samples(data):
    word = lambda at: struct.unpack_from('<I', data, at)[0]
    half = lambda at: struct.unpack_from('<H', data, at)[0]
    if data[:4] != b'XVAB' or word(4) != 0x101:
        raise RuntimeError('Only the observed XVAB version is supported')
    metadata_end = word(8)
    chunks = {}
    at = 12
    while at < metadata_end:
        if at+8 > metadata_end:
            raise RuntimeError('Truncated XVAB chunk')
        length = word(at+4)
        if at+8+length > metadata_end or data[at:at+4] in chunks:
            raise RuntimeError('XVAB chunk range/duplicate')
        chunks[data[at:at+4]] = (at+8, length)
        at += 8+length
    head, length = chunks[b'HEAD']
    if length != 28 or data[head] != 0:
        raise RuntimeError('Unexpected mapped XVAB header')
    body_start = (metadata_end+15) & ~15
    body = data[body_start:]
    if len(body) != word(head+4) or len(body) % 16:
        raise RuntimeError('XVAB body extent mismatch')
    table, table_size = chunks[b'TGIF']
    if word(head+8) != table or table_size != 4+4*half(table):
        raise RuntimeError('XVAB program table mismatch')
    pg, pg_size = chunks[b'TGPG']
    tones, tones_size = chunks[b'TGSP']
    programs = []
    for program in range(half(table)):
        address = word(table+4+4*program)
        if address < pg or address+8 > pg+pg_size:
            raise RuntimeError('XVAB program record bounds')
        count, first = half(address+2), word(address+4)
        if first < tones or first+count*36 > tones+tones_size:
            raise RuntimeError('XVAB tone record bounds')
        for tone in range(count):
            record = first+tone*36
            offset = word(record+0x14)  # RXSNDDRV 0x7f70 -> 0x3d1c.
            if offset >= len(body) or offset % 16:
                raise RuntimeError('XVAB sample pointer bounds')
            end = offset
            while end+16 <= len(body):
                end += 16
                if body[end-15] & 1:
                    break
            else:
                raise RuntimeError('XVAB sample has no end block')
            programs.append({'program': program, 'tone': tone, 'offset': offset,
                             'bytes': end-offset, 'key_low': data[record+4], 'key_high': data[record+5]})
    return body_start, body, programs

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--output', type=Path, default=Path('local/audio-validation-v2'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    pcsx2 = build()
    bank = Path('local/audio-input/SNDDATA/BGM/EVS_MSH0.XV').read_bytes()
    body_start, body, tones = opening_samples(bank)
    spu = state_spuram(Path('local/reference-intro-600-v2.state.zip'))
    address = spu.find(body)
    if address < 0:
        raise RuntimeError('Opening bank body not present verbatim in reference SPU RAM')
    cases = {}
    for tone in tones:
        offset = tone['offset']
        cases['sample-%06x' % offset] = body[offset:offset+tone['bytes']]
    synthetic = bytearray(16)
    for predictor in range(5):
        for shift in range(16):
            synthetic.extend(bytes([(predictor << 4) | shift, 0]) +
                             bytes((i*37+shift*17+predictor*29)&255 for i in range(14)))
    synthetic[-15] = 1
    cases['synthetic'] = synthetic
    report = {'scope': "Sample decode before pitch/envelope/mixing; PCSX2 equivalence, not hardware proof",
              'pcsx2_revision': REVISION, 'pcsx2_mixer_sha256': SOURCE_SHA256,
              'bank_sha256': sha(bank), 'body_offset': body_start, 'body_bytes': len(body),
              'body_sha256': sha(body), 'reference_spu_address': address,
              'reference_spuram_sha256': sha(spu), 'tones': tones, 'cases': []}
    for name, blocks in cases.items():
        source = args.output/(name+'.adpcm')
        native, reference = args.output/(name+'.native.pcm'), args.output/(name+'.pcsx2.pcm')
        play = args.output/(name+'.play.pcm')
        write_exact(source, blocks)
        subprocess.run([str(args.native), 'spu-sample', str(source), str(native)], check=True)
        subprocess.run([str(pcsx2), str(source), str(reference)], check=True)
        subprocess.run(['build/play-research/okage-oracle/okage-pose-oracle', 'spu-sample',
                        'local/extracted/SCUS_971.29', str(source), str(play)], check=True)
        pcm = native.read_bytes()
        if pcm != reference.read_bytes() or len(pcm) != len(blocks)//16*56:
            raise RuntimeError(name+': reference/native sample mismatch')
        report['cases'].append({'name': name, 'input_sha256': sha(blocks),
                                'samples': len(pcm)//2, 'pcm_sha256': sha(pcm),
                                'pcsx2_exact': True,
                                'play_differing_samples': sum(a!=b for a,b in zip(struct.iter_unpack('<h',pcm),struct.iter_unpack('<h',play.read_bytes()))),
                                'play_max_absolute_error': max(abs(a[0]-b[0]) for a,b in zip(struct.iter_unpack('<h',pcm),struct.iter_unpack('<h',play.read_bytes())))})
    # Diagnostic rate only: the original note pitch and envelope are not applied.
    diagnostic = args.output/'sample-000000.native.pcm'
    wav_path = args.output/'sample-000000-diagnostic-48000.wav'
    if not wav_path.exists():
        with wave.open(str(wav_path), 'wb') as wav:
            wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(48000)
            wav.writeframes(diagnostic.read_bytes())
    report['native_sha256'] = sha(args.native.read_bytes())
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
