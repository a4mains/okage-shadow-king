#!/usr/bin/env python3
"""Compare the five opening narration cues with pinned SPU voice kernels.

Direct key-on and fixed parameters: this does not model original stream-buffer
startup, DMA latency, shared voice allocation, wet routing or physical output.
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
from verify_finite_stream import finite_sample

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,default=Path('build/native/okage-assets'))
    p.add_argument('--output',type=Path,default=Path('local/stream-audio-validation'))
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    source=Path('local/audio-input/SNDDATA/SE/SET10.XWS');bank=source.read_bytes()
    digest=lambda data:hashlib.sha256(data).hexdigest()
    if digest(bank)!='84062be8c8b4d10d036e4c4ee4897a08cf875d57abe1bbd71748258ca887200a':
        raise ValueError('Expected observed SET10 revision')
    reference=build();cases=[]
    # Fixed offsets follow the original-registration comparison, not native CLI.
    for i,(offset,size) in enumerate([(0,31040),(31040,135776),(166816,136944),(303760,122896),(426656,83696)]):
        sample=args.output/f'cue-{i}.adpcm';write_exact(sample,finite_sample(bank[0x110+offset:0x110+offset+size]))
        frames=280000;original=args.output/f'cue-{i}.reference.pcm';native=args.output/f'cue-{i}.native.pcm'
        if not original.exists():
            subprocess.run([str(reference),'voice',str(sample),'32768','32806','3763','127','480000',str(frames),str(original)],check=True)
        metadata=list(map(int,subprocess.check_output([str(args.native),'stream-cue',str(source),str(i),'1','128',str(frames),str(native)],text=True).split()))
        if metadata!=[0x110+offset,size,44100,3763,127,127,0x8000,0x8026,0]:
            raise RuntimeError('Cue parameters or final voice state differ')
        data=native.read_bytes()
        if data!=original.read_bytes():raise RuntimeError(f'Cue {i} DSP mismatch')
        samples=struct.unpack('<%dh'%(len(data)//2),data)
        cases.append({'cue':i,'frames':frames,'exact':True,'sha256':digest(data),'peak':max(abs(x) for x in samples)})
    report={'scope':__doc__.strip(),'source_sha256':digest(bank),'reference_revision':REVISION,
            'reference_source_hashes':HASHES,'cases':cases,
            'parameter_evidence':'Original EE 0x176738/0x1769b0/0x172bf8 and IOP 0x9398/0x9770; first cue also observed in player1460 SPU core1/channel14'}
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode());print(json.dumps(report,indent=2))

if __name__=='__main__':main()
