#!/usr/bin/env python3
"""Native pitch versus the owned LIBSD function executed by the isolated IOP oracle."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact

def sha(b):
    return hashlib.sha256(b).hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native',type=Path,default=Path('build/native/okage-assets'))
    parser.add_argument('--output',type=Path,default=Path('local/music-pitch-validation'))
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    library=Path('local/extracted/MODULES/LIBSD.IRX')
    if sha(library.read_bytes())!='71596ef4029f672205e1523dedbd8dacfe5dfa50132c8a747b81289cd24f1db6':
        raise RuntimeError('Pinned LIBSD required')
    values=[0,1,64,127]
    fixture=b''.join(struct.pack('<4I',center,fine,key,fraction) for center in range(128)
                     for key in range(128) for fine in values for fraction in values)
    source,native,original=(args.output/name for name in ['fixture.bin','native.bin','original.bin'])
    write_exact(source,fixture)
    subprocess.run(['build/play-research/okage-oracle/okage-pose-oracle','iop-pitch',str(library),str(source),str(original)],check=True)
    subprocess.run([str(args.native),'music-pitch',str(source),str(native)],check=True)
    result=native.read_bytes()
    if result!=original.read_bytes() or len(result)!=len(fixture)//4:
        raise RuntimeError('Native/original pitch mismatch')
    report={'scope':'LIBSD0x33a0 nonnegative fine/key-fine inputs, 128 centers x128 keys x4 fines x4 fractions; no voice playback claim',
            'libsd_sha256':sha(library.read_bytes()),'fixture_sha256':sha(fixture),
            'output_sha256':sha(result),'cases':len(result)//4,'exact':True,
            'harness':'Original module loaded at address0, no external calls; synthetic return breakpoint only'}
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps(report,indent=2))

if __name__=='__main__':
    main()
