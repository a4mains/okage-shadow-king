#!/usr/bin/env python3
"""Compare the owned kitchen footstep with pinned SPU voice kernels.

Direct key-on at recovered fixed pitch/gain; excludes original stream buffering,
shared voice allocation, latency, wet routing and physical listening.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from build_voice_reference import build, HASHES
from build_spu_reference import REVISION
from verify_event import write_exact
from verify_finite_stream import finite_sample

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--reference', type=Path)
    p.add_argument('--output', type=Path, default=Path('local/footstep-audio-validation-v1'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = Path('local/audio-input/SNDDATA/SE/ALL.XWS')
    bank = source.read_bytes()
    digest = lambda data: hashlib.sha256(data).hexdigest()
    if digest(bank) != 'a422f83c70125c775cf234fdb195a4590253bcebf87d0fcf65727eaa5d5fea1a':
        raise RuntimeError('Expected observed ALL.XWS revision')
    sample = args.output/'footstep35.adpcm'
    write_exact(sample, finite_sample(bank[335808:335808+13568]))
    reference = (args.reference or args.output)/'reference.pcm'
    if args.reference:
        if (args.reference/sample.name).read_bytes() != sample.read_bytes():
            raise RuntimeError('Reference sample differs')
    else:
        subprocess.run([str(build()), 'voice', str(sample), '32768', '32806', '2730',
                        '59', '480000', '48000', str(reference)], check=True)
    native = args.output/'native.pcm'
    metadata = list(map(int, subprocess.check_output([str(args.native), 'stream-cue', str(source),
        '35', '1', '60', '48000', str(native)], text=True).split()))
    if metadata != [335808, 13568, 32000, 2730, 59, 59, 32768, 32806, 0]:
        raise RuntimeError('Footstep parameters or final voice state differ')
    pcm = native.read_bytes()
    if pcm != reference.read_bytes() or len(pcm) != 48000*4:
        raise RuntimeError('Footstep DSP comparison failed')
    report = {'scope': __doc__.strip(), 'cue': 35, 'stereo_frames': 48000, 'exact': True,
              'source_sha256': digest(bank), 'pcm_sha256': digest(pcm),
              'native_sha256': digest(args.native.read_bytes()),
              'reference_revision': REVISION, 'reference_source_hashes': HASHES,
              'parameter_evidence': ['EE 0x12df80 tables', 'EE 0x1769b0/0x176738/0x172bf8', 'IOP 0x9398/0x9770'],
              'endpoint': 'EE producer clears source flags, pads to64 bytes and appends its64-byte stop sequence; verified separately by verify_finite_stream.py.'}
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print('Footstep 35: 48000 stereo frames exact; native voice terminated')

if __name__ == '__main__':
    main()
