#!/usr/bin/env python3
"""Compare both original block-bound compiler modes on owned shadow XMDs.

Runs unchanged0x181e40 after original hierarchy construction/binding. Compares
every center/radius/index word. This is not field visibility or raster proof.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time
import math
from verify_event import write_exact
from verify_map_effect import ELF_SHA

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def compare(original,native):
    if not original or len(original)%4 or original!=native:
        raise ValueError('Model bound word/extent mismatch')
    words=struct.unpack('<%dI'%(len(original)//4),original);at=1
    blocks=words[0];total=0
    if not 0<blocks<=4096:raise ValueError('Model bound block count')
    for _ in range(blocks*2):
        if at>=len(words):raise ValueError('Missing model bound count')
        count=words[at];at+=1
        if not 0<count<=4096 or at+count*6>len(words):raise ValueError('Model bound count/extent')
        for _ in range(count):
            floats=struct.unpack('<5f',struct.pack('<5I',*words[at:at+5]))
            if not all(math.isfinite(f) for f in floats) or floats[3]!=1 or floats[4]<0 or words[at+5]>=4096:
                raise ValueError('Invalid model bound')
            at+=6
        total+=count
    if at!=len(words):raise ValueError('Trailing model bound words')
    return dict(blocks=blocks,bounds_exact=total,words_exact=total*6)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--oracle',type=Path,default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference',type=Path,default=Path('local/research/shadow-visibility-v1'))
    p.add_argument('--assets',type=Path,default=Path('local/xpf-decoded/FLDDATA/OBJ'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);a.reference.mkdir(parents=True,exist_ok=True)
    elf=Path('local/extracted/SCUS_971.29')
    if digest(elf)!=ELF_SHA:raise ValueError('Different owned ELF')
    cases=[]
    for name in ['HERO','MAM','IMO','PAP']:
        source=a.assets/(name+'.XPF')/'shadow.xmd';main=a.assets/(name+'.XPF')/'a.xmd'
        original=a.reference/(name+'-bounds-original-v1.bin');manifest=original.with_suffix('.run.json')
        inputs={str(path):digest(path) for path in [main,source]}
        if not original.exists():
            if manifest.exists():raise ValueError('Retained original failure; investigate before retry')
            cmd=[str(a.oracle),'shadow-bounds',str(elf),str(main),str(source),str(original)]
            start=time.monotonic()
            with original.with_suffix('.log').open('x') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
            run=dict(command=cmd,exit_code=r.returncode,elapsed_seconds=time.monotonic()-start,
                     binary_sha256=digest(a.oracle),elf_sha256=ELF_SHA,input_sha256=inputs)
            if not r.returncode:run['output_sha256']=digest(original)
            write_exact(manifest,(json.dumps(run,indent=2)+'\n').encode())
            if r.returncode:raise RuntimeError('Original model bounds failed')
        run=json.loads(manifest.read_text())
        if run['exit_code'] or run['elf_sha256']!=ELF_SHA or run['input_sha256']!=inputs or run['output_sha256']!=digest(original):
            raise ValueError('Changed/incomplete original model bounds')
        native=a.output/(name+'-native.bin');cmd=[str(a.native),'shadow-bounds',str(source),str(native)]
        subprocess.run(cmd,check=True)
        cases.append(dict(name=name,original_run=run,native_command=cmd,**compare(original.read_bytes(),native.read_bytes())))
    write_exact(a.output/'report.json',(json.dumps(dict(scope=__doc__,native_sha256=digest(a.native),cases=cases),indent=2)+'\n').encode())
    print(json.dumps({c['name']:{k:v for k,v in c.items() if k.endswith('_exact')} for c in cases}))

if __name__=='__main__':main()
