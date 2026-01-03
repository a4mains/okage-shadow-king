#!/usr/bin/env python3
"""Verify pinned music bank note parameters from a completed original snapshot."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zipfile
import zlib
from verify_event import write_exact

def sha(b):
    return hashlib.sha256(b).hexdigest()

def iop_ram(path):
    with zipfile.ZipFile(path) as archive:
        info = archive.getinfo('iop_ram')
    if info.file_size != 4*1024*1024 or info.compress_type != 93:
        raise RuntimeError('Unexpected IOP snapshot extent/compression')
    with path.open('rb') as stream:
        stream.seek(info.header_offset)
        header = stream.read(30)
        if header[:4] != b'PK\x03\x04':
            raise RuntimeError('Invalid ZIP header')
        name, extra = struct.unpack_from('<HH', header, 26)
        stream.seek(name+extra, 1)
        compressed = stream.read(info.compress_size)
    b = subprocess.run(['zstd', '-d', '-c'], input=compressed, stdout=subprocess.PIPE, check=True).stdout
    if len(b) != info.file_size or zlib.crc32(b) != info.CRC:
        raise RuntimeError('IOP snapshot checksum')
    return b

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--output', type=Path, default=Path('local/music-bank-validation'))
    p.add_argument('--bank', type=Path, default=Path('local/audio-input/SNDDATA/BGM/EVS_MSH0.XV'))
    p.add_argument('--reference', type=Path, default=Path('local/reference-music-600-v4'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    bank_path = args.bank
    bank = bank_path.read_bytes()
    if sha(bank) not in ('1c0428b3fcaba0f822da47ff6374ac0bd1e5d90cec1bd2f3dadc567cd95c3654',
                         'a2c11debcde59c737fe9217821d4cbaf27f42a83eb4c202c2ea7a97ab654df85'):
        raise RuntimeError('Bank hash mismatch')
    ref = args.reference
    if 'Stopped: vblanks=' not in ref.with_suffix('.log').read_text() or 'exit_request=0' not in ref.with_suffix('.log').read_text():
        raise RuntimeError('Incomplete original run')
    manifest=ref.with_suffix('.run.json')
    if manifest.exists() and json.loads(manifest.read_text())['exit_code'] != 0:
        raise RuntimeError('Failed original process')
    ram = iop_ram(ref.with_suffix('.state.zip'))
    irx = Path('local/extracted/MODULES/RXSNDDRV.IRX').read_bytes()
    if sha(irx) != '7fe8b5a33c2f8700d1f964cf5ce2f66942f8f97d952e86e6dc0ad5d4f74becb2':
        raise RuntimeError('Driver hash mismatch')
    candidates = [i for i in range(0, len(ram)-233424, 16)
                  if all(ram[i+off:i+off+16] == irx[176+off:176+off+16] for off in [0x4c9c, 0x4ea0, 0x7f70])]
    if len(candidates) != 1:
        raise RuntimeError('Cannot identify runtime driver')
    base = candidates[0]
    word = lambda at: struct.unpack_from('<I', ram, at)[0]
    half = lambda at: struct.unpack_from('<H', ram, at)[0]
    bw = lambda at: struct.unpack_from('<I', bank, at)[0]
    bh = lambda at: struct.unpack_from('<H', bank, at)[0]
    trace = ref.with_suffix('.bin').read_bytes()
    rows = list(struct.iter_unpack('<184I', trace))
    slot = rows[-1][4]
    if word(slot+8) & 8 or ram[slot+0x1e]:
        raise RuntimeError('Snapshot has paused/pending global music controls')
    note = word(slot+0x2ac)
    fixture, expected, cases, visited = bytearray(), bytearray(), [], set()
    while note:
        if note in visited or len(visited)>128:
            raise RuntimeError('Music note list cycle/extent')
        visited.add(note)
        channel = word(note+8)
        if channel < slot+44 or channel >= slot+44+16*40 or (channel-slot-44)%40:
            raise RuntimeError('Invalid music channel pointer')
        program, key, velocity, idx = ram[channel+5], ram[note+15], ram[note+16], ram[note+12]
        driver_note = base+0x2ac54+44*idx
        if idx>=64 or word(driver_note+8)!=3 or ram[driver_note+16]:
            raise RuntimeError('Uncovered/pending native note flags')
        v = word(driver_note+36)
        voice_seen = set()
        while v:
            if v in voice_seen or len(voice_seen)>48:
                raise RuntimeError('Voice list cycle/extent')
            voice_seen.add(v)
            tone, hw = word(v+8), ram[v+32]
            if hw >= 48 or ram[v+33]:
                raise RuntimeError('Unapplied original voice parameters')
            # Independent file offsets from HEAD/TGIF and the original tone record bytes.
            table = bw(20+8)
            pg = bw(table+4+4*program)
            first, count = bw(pg+4), bh(pg+2)
            matches = [i for i in range(count) if bank[first+i*36:first+(i+1)*36] == ram[tone:tone+36]]
            if len(matches)!=1:
                raise RuntimeError('Original tone does not resolve uniquely in owned bank')
            out = base+0x198d0+44*hw
            bank_id = word(driver_note+4)
            sample_base = word(base+0x184d8+32*bank_id)
            fixture.extend(struct.pack('<4I',program,matches[0],key,velocity)+ram[channel:channel+40]+struct.pack('<2I',half(slot+32),half(slot+34)))
            values = (word(out+4)-sample_base,half(out+12),half(out+14),half(out+16),half(out+18),half(out+2))
            expected.extend(struct.pack('<6I',*values))
            cases.append({'program':program,'tone':matches[0],'key':key,'velocity':velocity,'voice':hw,
                          'sample_offset':values[0],'adsr1':values[1],'adsr2':values[2],'left':values[3],'right':values[4],'pitch':values[5]})
            v = word(v)
        note = word(note)
    if not cases:
        raise RuntimeError('No original voices observed')
    source, output = args.output/'fixture.bin', args.output/'native.bin'
    write_exact(source, fixture)
    subprocess.run([str(args.native),'music-tone',str(bank_path),str(source),str(output)],check=True)
    if output.read_bytes()!=expected:
        print('expected',list(struct.iter_unpack('<6I',expected)))
        print('native',list(struct.iter_unpack('<6I',output.read_bytes())))
        raise RuntimeError('Original/native voice parameter mismatch')
    report = {'scope':'Tone/sample/envelope/volume/pitch parameters for observed active voices; given original note/channel inputs, not a native scheduler or mixer',
              'bank_sha256':sha(bank),'iop_ram_sha256':sha(ram),'reference_trace_sha256':sha(trace),
              'fixture_sha256':sha(fixture),'output_sha256':sha(expected),'cases':cases,'exact':True}
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps(report,indent=2))

if __name__=='__main__':
    main()
