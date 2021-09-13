#!/usr/bin/env python3
"""Compare original effect noise initialization and its cube-face consumers.

Explicit seeds and captured allocator state are research preconditions. Native
inputs are seeds only. This is not full template-loader/startup RNG validation.
Triangles compare exact multisets including attributes and multiplicity; retail
strip order/winding are not required to match and no raster equality is claimed.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import subprocess
import time
from verify_event import write_exact
from verify_map_effect import digest, ELF_SHA
from verify_shadow_drawing import Reader, packets

SEEDS=(1,0x1234,0x7fffffff)

def fixture():
    return b''.join(struct.pack('<2I',i,seed) for i,seed in enumerate(SEEDS))

def triangles(vertices):
    return Counter(tuple(sorted(vertices[i:i+3])) for i in range(0,len(vertices),3))

def compare(original,native):
    r=Reader(original);n=Reader(native);total=0;counts=[]
    for case,seed in enumerate(SEEDS):
        header=r.words(4)
        if header[:2]!=(case,seed) or header[3]!=384 or n.words(4)!=header:
            raise ValueError('Noise fixture identity/RNG')
        grid=r.words(384)
        if any(x>6 for x in grid) or n.words(384)!=grid:raise ValueError('Noise grid mismatch')
        case_counts=[]
        for shape in range(10,17):
            got,rng,pieces=r.words(3);other,actual_rng,count=n.words(3)
            if (got,rng)!=(shape,header[2]) or (other,actual_rng)!=(got,rng):
                raise ValueError('Noise consumer changed RNG/identity')
            if not 1<=pieces<=32:raise ValueError('Noise geometry piece count')
            expected=[]
            for _ in range(pieces):expected.extend(packets(r,1,False))
            wanted=grid.count(shape-10)*6
            if count!=wanted or len(expected)!=wanted:raise ValueError('Noise partition vertex count')
            actual=list(struct.iter_unpack('<12f',n.take(count*48)))
            if triangles(expected)!=triangles(actual):raise ValueError(f'Noise shape{shape} geometry differs in case{case}')
            total+=count;case_counts.append(count)
        if sum(case_counts)!=384*6:raise ValueError('Noise partitions do not cover cube')
        counts.append(case_counts)
    r.finish();n.finish()
    return dict(grid_words_exact=384*len(SEEDS),triangle_vertices_exact=total,vertices_per_partition=counts)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--oracle',type=Path,default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference',type=Path,default=Path('local/research/effect-template-v1'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);a.reference.mkdir(parents=True,exist_ok=True)
    b=a.reference;elf=Path('local/extracted/SCUS_971.29');ram=Path('local/research/shadow-transition-v1/before-shadow.ram')
    if digest(elf)!=ELF_SHA:raise ValueError('Different retail ELF')
    inp=b/'noise-fixture-v1.bin';write_exact(inp,fixture())
    original=b/'noise-original-v1.bin';manifest=b/'noise-original-v1.run.json'
    if not original.exists():
        if manifest.exists():raise ValueError('Retained failed original attempt; investigate before using a fresh reference directory')
        cmd=[str(a.oracle),'effect-noise',str(elf),str(ram),str(inp),str(original)]
        t=time.monotonic()
        with (b/'noise-original-v1.log').open('w') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
        run=dict(command=cmd,exit_code=r.returncode,elapsed_seconds=time.monotonic()-t,
                 binary_sha256=digest(a.oracle),elf_sha256=ELF_SHA,input_sha256=digest(inp),ram_sha256=digest(ram))
        if not r.returncode:run['output_sha256']=digest(original)
        write_exact(manifest,(json.dumps(run,indent=2)+'\n').encode())
        if r.returncode:raise RuntimeError('Original effect noise probe failed')
    run=json.loads(manifest.read_text())
    if run['exit_code'] or run['elf_sha256']!=ELF_SHA or any(run[k]!=digest(path) for k,path in
            [('input_sha256',inp),('ram_sha256',ram),('output_sha256',original)]):
        raise ValueError('Changed/incomplete original noise corpus')
    out=a.output/'native.bin';cmd=[str(a.native),'effect-noise',str(inp),str(out)]
    subprocess.run(cmd,check=True)
    result=compare(original.read_bytes(),out.read_bytes())
    report=dict(scope=__doc__,elf_sha256=ELF_SHA,original_run=run,native_command=cmd,
                native_sha256=digest(a.native),**result)
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(f"{result['grid_words_exact']} grid words and {result['triangle_vertices_exact']} partition vertices exact")

if __name__=='__main__':main()
