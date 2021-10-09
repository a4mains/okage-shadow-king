#!/usr/bin/env python3
"""Compare original block selection/sorting with native camera-space bounds.

Synthetic inputs exercise unchanged0x1857bc..0x185a98 and0x185ab0..0x185c1c,
without block-bone masks. This does not validate original pose/packet setup or
field raster. Own-model bound construction has its separate original comparison.
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

def fixtures():
    settings=[800,1,.47,2048,2048,101,16777115,3,2048,3,1024,0,0,320,112]
    identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
    cases=[]
    def add(blocks,poses=None,culling=1,screen=None):
        cases.append(dict(id=len(cases)+1,culling=culling,blocks=blocks,poses=poses or [identity],screen=screen or settings))
    def bound(x=0,y=0,z=10,r=0,node=0):return [x,y,z,1,r,node]
    for radius in [0,.25,2]:
        for offset in [-.0001,0,.0001]:
            for sign in [-1,1]:
                add([[bound(x=sign*(4+radius+offset),r=radius)]])
                add([[bound(y=sign*((112/.47/800)*10+radius+offset),r=radius)]])
            add([[bound(z=3-radius+offset,r=radius)]])
            add([[bound(z=1024+radius+offset,r=radius)]])
    add([[bound(z=10.125)],[bound(z=10.875)],[bound(z=10.125)],[bound(z=9.99)]])
    add([[bound(x=1000,z=10),bound(z=4)],[bound(z=7)],[bound(z=7)]])
    add([[bound(z=-30)],[bound(x=100000,z=20)],[bound(z=2000)]],culling=0)
    add([[bound(z=-30)],[bound(x=100000,z=20)],[bound(z=2000)]])
    scaled=[2,.25,0,0,.5,.75,.125,0,0,-.25,1.5,0,3,-2,9,1]
    moved=identity.copy();moved[12:15]=[-6,5,3]
    rng=random.Random(1856)
    for i in range(32):
        blocks=[]
        for j in range(7):
            blocks.append([bound(rng.uniform(-30,30),rng.uniform(-20,20),rng.uniform(-10,70),rng.uniform(.01,4),rng.randrange(3)) for _ in range(1+j%3)])
        screen=settings.copy()
        if i%2:screen[9:11]=[1,48];screen[7]=6
        add(blocks,[identity,scaled,moved],culling=int(i%5!=0),screen=screen)
    data=bytearray()
    for case in cases:
        data+=struct.pack('<4I15f',case['id'],case['culling'],len(case['blocks']),len(case['poses']),*case['screen'])
        for pose in case['poses']:data+=struct.pack('<16f',*pose)
        for block in case['blocks']:
            data+=struct.pack('<I',len(block))
            for b in block:data+=struct.pack('<5fI',*b)
    return bytes(data),cases

def compare(original,native,cases):
    if original!=native or len(original)%4:raise ValueError('Model visibility word/extent mismatch')
    words=struct.unpack('<%dI'%(len(original)//4),original);at=0;selected=empty=0
    for case in cases:
        if at+2>len(words) or words[at]!=case['id']:raise ValueError('Missing/misaligned visibility case')
        count=words[at+1];at+=2
        if count>len(case['blocks']) or at+count*2>len(words):raise ValueError('Visibility result extent')
        seen=set();previous=math.inf;previous_index=-1
        for _ in range(count):
            index=words[at];depth=struct.unpack('<f',struct.pack('<I',words[at+1]))[0];at+=2
            if index>=len(case['blocks']) or index in seen or not math.isfinite(depth):raise ValueError('Invalid visibility entry')
            if depth>previous or (depth==previous and index<previous_index):raise ValueError('Unstable visibility order')
            previous=depth;previous_index=index;seen.add(index)
        if not case['culling'] and count!=len(case['blocks']):raise ValueError('Disabled culling omitted blocks')
        selected+=count;empty+=count==0
    if at!=len(words):raise ValueError('Trailing visibility output')
    return dict(cases_exact=len(cases),selected_blocks_exact=selected,empty_cases_exact=empty,words_exact=len(words))

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--oracle',type=Path,default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference',type=Path,default=Path('local/research/shadow-visibility-v2'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);a.reference.mkdir(parents=True,exist_ok=True)
    data,cases=fixtures();fixture=a.reference/'visibility-fixture-v1.bin';write_exact(fixture,data)
    elf=Path('local/extracted/SCUS_971.29')
    if digest(elf)!=ELF_SHA:raise ValueError('Different owned ELF')
    original=a.reference/'visibility-original-v1.bin';manifest=original.with_suffix('.run.json')
    if not original.exists():
        if manifest.exists():raise ValueError('Retained original failure; investigate before retry')
        cmd=[str(a.oracle),'shadow-visibility',str(elf),str(fixture),str(original)]
        start=time.monotonic()
        with original.with_suffix('.log').open('x') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
        run=dict(command=cmd,exit_code=r.returncode,elapsed_seconds=time.monotonic()-start,
                 binary_sha256=digest(a.oracle),elf_sha256=ELF_SHA,input_sha256=digest(fixture))
        if not r.returncode:run['output_sha256']=digest(original)
        write_exact(manifest,(json.dumps(run,indent=2)+'\n').encode())
        if r.returncode:raise RuntimeError('Original block visibility failed')
    run=json.loads(manifest.read_text())
    if run['exit_code'] or run['elf_sha256']!=ELF_SHA or run['input_sha256']!=digest(fixture) or run['output_sha256']!=digest(original):
        raise ValueError('Changed/incomplete original visibility')
    native=a.output/'native.bin';cmd=[str(a.native),'shadow-visibility',str(fixture),str(native)]
    subprocess.run(cmd,check=True)
    report=dict(scope=__doc__,original_run=run,native_command=cmd,native_sha256=digest(a.native),**compare(original.read_bytes(),native.read_bytes(),cases))
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps({k:v for k,v in report.items() if k.endswith('_exact')}))

if __name__=='__main__':main()
