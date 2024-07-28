#!/usr/bin/env python3
"""Compare a reached FORM loop with original source traversal and dry DSP.

RAM fixtures and emulator binaries are research-only. The runtime loads only the
owned RXWS bank. This does not establish mixed audio, DMA timing or reverb parity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from build_voice_reference import build
from verify_finite_stream import audit_original, bank_payload

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def audit_source(payload, offset, loop, data, rows):
    if len(rows) < 32 or len(rows) % 16:
        raise ValueError('Loop source row extent')
    source, start, size, repeat = struct.unpack_from('<4I', rows)
    if source >= 256 or (start, size, repeat) != (offset, len(payload), loop):
        raise ValueError('Loop source constructor metadata')
    if data != payload + payload[loop:] * 3:
        raise ValueError('Original source loop bytes differ')
    cursor = 0
    first_pointer = None
    for end, pointer, count, eof in struct.iter_unpack('<4I', rows[16:]):
        if eof or not 0 < count <= 4096 or end != cursor + count:
            raise ValueError('Loop source availability/EOF')
        relative = cursor if cursor < size else loop + (cursor-size) % (size-loop)
        if first_pointer is None:
            first_pointer = pointer
        if pointer != first_pointer + relative or count > size-relative:
            raise ValueError('Loop source cursor/branch')
        cursor = end
    if cursor != len(data):
        raise ValueError('Incomplete loop traversal')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--prepared-original', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir()
    bank_path = Path('local/audio-input/SNDDATA/SE/ALL.XWS')
    if digest(bank_path) != 'a422f83c70125c775cf234fdb195a4590253bcebf87d0fcf65727eaa5d5fea1a':
        raise ValueError('Unexpected ALL bank')
    bank = bank_path.read_bytes()
    payload, absolute, rate = bank_payload(bank, 29)
    record = 0x350
    offset, size, loop = struct.unpack_from('<3I', bank, record+16)
    if bank[record:record+4] != bytes([0,28,3,128]) or size != len(payload) or loop != 16:
        raise ValueError('Unexpected reached cue29 FORM')
    rows_path = Path(str(args.original)+'.rows.bin')
    audit_source(payload, offset, loop, args.original.read_bytes(), rows_path.read_bytes())
    commands = Path(str(args.prepared_original)+'.commands.bin')
    context = Path(str(args.prepared_original)+'.context.bin')
    linear = audit_original(args.original.read_bytes(), args.prepared_original.read_bytes(),
                            commands.read_bytes(), context.read_bytes())
    linear_path = args.output/'original-linear.adpcm'
    linear_path.write_bytes(linear)
    reference = build()
    frames = 400000
    pitch = rate*4096//48000
    original_pcm = args.output/'original-unrolled.pcm'
    native_pcm = args.output/'native-loop.pcm'
    commands_run = [
        [str(reference), 'voice', str(linear_path), '0x8000', '0x8026', str(pitch),
         '127', str(frames), str(frames), str(original_pcm)],
        [str(args.native), 'stream-cue', str(bank_path), '29', '1', '128',
         str(frames), str(native_pcm)],
    ]
    for command in commands_run:
        subprocess.run(command, check=True)
    if native_pcm.read_bytes() != original_pcm.read_bytes():
        raise ValueError('Loop PCM differs from original traversal/DSP')
    files = [bank_path, args.native, args.original, rows_path, args.prepared_original,
             commands, context, reference, linear_path, native_pcm, original_pcm]
    report = {'scope': __doc__, 'cue':29, 'source_offset':offset, 'source_size':size,
              'source_loop_offset':loop, 'bank_payload_absolute':absolute,
              'source_passes':4, 'pcm_frames':frames, 'pcm_bytes':native_pcm.stat().st_size,
              'pitch':pitch, 'commands':commands_run,
              'artifacts':{str(p):digest(p) for p in files}}
    (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ['source_passes','pcm_frames','pcm_bytes']}))

if __name__ == '__main__':
    main()
