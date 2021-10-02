#!/usr/bin/env python3
"""Compare native Metal shadow pixels with original VU1 and pinned Vulkan GS.

Explicit synthetic prism camera/screen caches, background depth/colors and repeat
counts feed both paths. This validates the raster stage, not field configuration,
model assembly, complete original hardware fidelity or game progression.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time
from verify_event import write_exact
from verify_map_effect import ELF_SHA

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def fixture():
    result=[]
    for case in range(16):
        lower=[(0,0,3,0),(1,0,3,0),(0,0,4,0)]
        upper=[(x,y+1,z,f) for x,y,z,f in lower]
        screen=[(1800,1990,10000,128),(2048,1990,10000,128),(1800,2120,10000,128)]
        other=[(x+40,y-50,z+500,w) for x,y,z,w in screen]
        depths=[0,9800,9999,10000,10100,10300,10500,10700]
        if case>=12:
            screen=[(x+.0625,y+.3125,z+index*31,w) for index,(x,y,z,w) in enumerate(screen)]
            other=[(x+.3125,y+.0625,z-index*17,w) for index,(x,y,z,w) in enumerate(other)]
        row=struct.pack('<Ii4f',case,0,0,1,0,0)
        row+=b''.join(struct.pack('<3fI',*v) for v in lower+upper)
        row+=b''.join(struct.pack('<4f',*v) for v in screen+other)
        row+=struct.pack('<4I',231|(117<<8)|(33<<16),depths[case%8],0 if case<8 else 201|(46<<8)|(177<<16),1 if case<8 else 3)
        result.append(row)
    return b''.join(result)

def compare(original,native,inputs):
    frame=640*224*4
    if not inputs or len(inputs)%232 or len(inputs)>48*232:raise ValueError('Shadow raster fixture extent')
    if len(original)!=len(inputs)//232*frame or len(native)!=len(original):raise ValueError('Shadow raster image extent')
    cases=[]
    for case,at in enumerate(range(0,len(original),frame)):
        a,b=original[at:at+frame],native[at:at+frame]
        differences=[i for i,(x,y) in enumerate(zip(a,b)) if x!=y]
        rgb=struct.unpack_from('<I',inputs,case*232+54*4)[0]
        background=bytes((rgb&255,(rgb>>8)&255,(rgb>>16)&255,64))
        changed=sum(a[p:p+4]!=background for p in range(0,frame,4))
        cases.append(dict(case=case,different_bytes=len(differences),different_pixels=len({i//4 for i in differences}),
                          original_changed_pixels=changed,first_differences=[dict(x=i//4%640,y=i//4//640,channel=i%4,original=a[i],native=b[i]) for i in differences[:12]]))
    return cases

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--oracle',type=Path,default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference',type=Path,default=Path('local/research/shadow-raster-v1'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);a.reference.mkdir(parents=True,exist_ok=True)
    inputs=a.reference/'fixture-v1.bin';write_exact(inputs,fixture())
    elf=Path('local/extracted/SCUS_971.29')
    if digest(elf)!=ELF_SHA:raise ValueError('Different owned ELF')
    original=a.reference/'original-v1.rgba';manifest=a.reference/'original-v1.run.json'
    if not original.exists():
        if manifest.exists():raise ValueError('Retained original failure; investigate before retry')
        cmd=[str(a.oracle),'shadow-raster',str(elf),str(inputs),str(original)]
        start=time.monotonic()
        with (a.reference/'original-v1.log').open('w') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
        run=dict(command=cmd,exit_code=r.returncode,elapsed_seconds=time.monotonic()-start,binary_sha256=digest(a.oracle),elf_sha256=ELF_SHA,input_sha256=digest(inputs))
        if not r.returncode:run['output_sha256']=digest(original)
        write_exact(manifest,(json.dumps(run,indent=2)+'\n').encode())
        if r.returncode:raise RuntimeError('Original shadow raster failed')
    run=json.loads(manifest.read_text())
    if run['exit_code'] or run['elf_sha256']!=ELF_SHA or run['input_sha256']!=digest(inputs) or run['output_sha256']!=digest(original):raise ValueError('Changed/incomplete original raster')
    native=a.output/'native.rgba';cmd=[str(a.native),str(inputs),str(native)];subprocess.run(cmd,check=True)
    cases=compare(original.read_bytes(),native.read_bytes(),inputs.read_bytes())
    report=dict(scope=__doc__,original_run=run,native_command=cmd,native_sha256=digest(a.native),native_output_sha256=digest(native),cases=cases)
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps([{k:v for k,v in c.items() if k!='first_differences'} for c in cases]))
    if any(c['different_bytes'] for c in cases):raise ValueError('Native/original shadow raster differs; report retained')

if __name__=='__main__':main()
