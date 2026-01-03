#!/usr/bin/env python3
"""Compare native RXWS registration/lookup with the original EE implementation."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from prepare_assets import ELF_HASH
from verify_event import write_exact

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,default=Path('build/native/okage-assets'))
    p.add_argument('--input',type=Path,default=Path('local/audio-input/SNDDATA/SE/SET10.XWS'))
    p.add_argument('--output',type=Path,default=Path('local/stream-bank-validation'))
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    original,native=args.output/'reference.bin',args.output/'native.bin'
    subprocess.run(['build/play-research/okage-oracle/okage-pose-oracle','stream-bank',
                    'local/extracted/SCUS_971.29',str(args.input),str(original)],check=True)
    subprocess.run([str(args.native),'stream-bank',str(args.input),str(native)],check=True)
    actual=native.read_bytes()
    if actual!=original.read_bytes():raise RuntimeError('RXWS original/native registration mismatch')
    report={'elf_sha256':ELF_HASH,'input':str(args.input),'input_sha256':hashlib.sha256(args.input.read_bytes()).hexdigest(),
            'exact':True,'compared_words':len(actual)//4,'sha256':hashlib.sha256(actual).hexdigest(),
            'scope':'FORM/FTXT/MARK/BRCH/BODY registration offsets, BODY extent, record count, all FORM lookups and two invalid indices. No stream playback/timing claim.',
            'oracle':'Original 0x174950/0x175038/0x174ff0/0x174be8/0x174f28; semaphore entry/exit no-ops, initially empty slots'}
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode());print(json.dumps(report,indent=2))

if __name__=='__main__':main()
