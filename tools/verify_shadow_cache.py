#!/usr/bin/env python3
"""Compare full perspective setup and shadow VU1 camera/screen caches exactly.

Explicit settings, matrices and vertices feed native arithmetic and original EE
setup plus VU1 entries0/2/6/8/12/18/16/20. No RAM snapshots, raster comparison or
field integration are involved. The two output caches include original clip flags
and accumulated clip status, including the zero-W division boundary.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import time
from verify_event import write_exact
from verify_map_effect import ELF_SHA

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def fixture():
    rng=random.Random(0x219700)
    rows=[]
    for case in range(128):
        count=case%6+1
        settings=[800,1,.47,2048,2048,101,16777115,3,2048,3,1024,0,0,320,112]
        identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
        lower=identity.copy();upper=identity.copy();upper[4]=.3;upper[13]=1
        vertices=[(0,0,16,1),(1,2,32,1),(-3,1,8,1),(4,-5,256,1),(-2,-1,3,1),(0,0,1024,1)]
        if case in (1,2,3,4,5,6,7):
            vertices[0]=(0,0,[0,-0.,-1,3,1024,2048,.00001][case-1],1)
        if case==8:vertices=[(1000,0,16,1)]*6
        if case==9:vertices=[(0,-1000,16,1)]*6
        if case>=10:
            settings[0]=rng.uniform(200,1000);settings[1]=rng.uniform(.5,1.5);settings[2]=rng.uniform(.25,.75)
            settings[3]=rng.uniform(1900,2100);settings[4]=rng.uniform(1900,2100)
            settings[8]=[2048,4096,1024][case%3];settings[10]=[1024,512,2048][case%3]
            settings[11:13]=[(0,0),(50,100),(4096,16384)][case%3]
            lower[4]=rng.uniform(-.5,.5);lower[8]=rng.uniform(-.25,.25);lower[12]=rng.uniform(-10,10)
            upper[5]=rng.uniform(.25,1.5);upper[6]=rng.uniform(-.5,.5);upper[13]=rng.uniform(-10,10)
            vertices=[(rng.uniform(-100,100),rng.uniform(-100,100),rng.uniform(-10,2050),1) for _ in range(6)]
        rows.append(struct.pack('<2I71f',case,count,*settings,*lower,*upper,*(v for row in vertices for v in row)))
    return b''.join(rows)

def compare(original,native,inputs):
    if not inputs or len(inputs)%292 or len(inputs)>128*292:raise ValueError('Shadow cache fixture extent')
    rows=list(struct.iter_unpack('<2I71f',inputs))
    offset=words_exact=vertices=0
    statuses={-1:0,0:0,1:0}
    for case,row in enumerate(rows):
        identity,count=row[:2]
        if identity!=case or not 1<=count<=6 or not all(math.isfinite(v) for v in row[2:]):
            raise ValueError('Shadow cache fixture scope')
        size=(35+count*16)*4
        if len(original)-offset<size or len(native)-offset<size:raise ValueError('Shadow cache record extent')
        expected=struct.unpack_from('<%dI'%(size//4),original,offset)
        actual=struct.unpack_from('<%dI'%(size//4),native,offset)
        if expected[:2]!=(identity,count) or expected[2] not in (0xffffffff,0,1):
            raise ValueError('Original shadow cache identity/status')
        if expected!=actual:
            diff=[(i,hex(a),hex(b)) for i,(a,b) in enumerate(zip(expected,actual)) if a!=b]
            raise ValueError(f'Shadow cache case{case} differs: {diff[:8]}')
        flags=[expected[35+i*8+3] for i in range(count*2)]
        clip_union=0;clip_intersection=63
        for value in flags:
            if value>63:raise ValueError('Shadow cache clip flags')
            clip_union|=value;clip_intersection&=value
        status=(-1 if clip_intersection else 1) if clip_union else 0
        if expected[2]!=(status&0xffffffff):raise ValueError('Shadow cache clip aggregation')
        statuses[status]+=1
        offset+=size;words_exact+=size//4-2;vertices+=count*2
    if offset!=len(original) or offset!=len(native):raise ValueError('Shadow cache trailing records')
    return dict(cases=len(rows),setup_words_exact=len(rows)*32,cache_vertices_exact=vertices,
                data_words_exact=words_exact,clip_status_counts=statuses)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--oracle',type=Path,default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference',type=Path,default=Path('local/research/shadow-cache-v1'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    root=a.reference;root.mkdir(parents=True,exist_ok=True)
    inputs=root/'fixture-v1.bin';write_exact(inputs,fixture())
    elf=Path('local/extracted/SCUS_971.29')
    if digest(elf)!=ELF_SHA:raise ValueError('Different retail ELF')
    original=root/'original-v1.bin';manifest=root/'original-v1.run.json'
    if not original.exists():
        if manifest.exists():raise ValueError('Retained failed original probe; investigate first')
        cmd=[str(a.oracle),'shadow-cache',str(elf),str(inputs),str(original)]
        start=time.monotonic()
        with (root/'original-v1.log').open('w') as log:result=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
        run=dict(command=cmd,exit_code=result.returncode,elapsed_seconds=time.monotonic()-start,
                 binary_sha256=digest(a.oracle),elf_sha256=ELF_SHA,input_sha256=digest(inputs))
        if not result.returncode:run['output_sha256']=digest(original)
        write_exact(manifest,(json.dumps(run,indent=2)+'\n').encode())
        if result.returncode:raise RuntimeError('Original shadow cache failed')
    run=json.loads(manifest.read_text())
    if run['exit_code'] or run['elf_sha256']!=ELF_SHA or run['input_sha256']!=digest(inputs) or run['output_sha256']!=digest(original):
        raise ValueError('Changed/incomplete original shadow cache')
    native=a.output/'native.bin';cmd=[str(a.native),'shadow-cache',str(inputs),str(native)]
    subprocess.run(cmd,check=True)
    result=compare(original.read_bytes(),native.read_bytes(),inputs.read_bytes())
    report=dict(scope=__doc__,original_run=run,native_command=cmd,native_sha256=digest(a.native),**result)
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode());print(json.dumps(result))

if __name__=='__main__':main()
