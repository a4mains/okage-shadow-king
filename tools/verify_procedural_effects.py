#!/usr/bin/env python3
"""Verify first-use procedural43/45/55/56/62 geometry, tags, UVs and RNG.

Uses explicit seeds and a synthetic one-element EFD in the original oracle.
Native inputs are seeds and owned XIM dimensions, never captured vertices/RAM.
Exact ordered vertices include all attributes/tags and multiplicity. This does
not prove raster or session integration.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import subprocess
import time
from verify_event import write_exact
from verify_map_effect import digest,ELF_SHA
from verify_shadow_drawing import Reader
from verify_summon_drawing import unpack_records

CASES=tuple((shape,seed) for seed in (1,0x1234,0x7fffffff) for shape in (43,45,55,56,62))
COUNTS={43:24,45:24,55:240,56:240,62:60}

def fixture():return b''.join(struct.pack('<3I',i,shape,seed) for i,(shape,seed) in enumerate(CASES))

def piece(reader,textured):
    tags=[]
    for kind in range(2):
        count=reader.words(1)[0]
        if count>512:raise ValueError('Procedural tag count')
        values=reader.words(count)
        if any(x>255 for x in values):raise ValueError('Procedural tag value')
        tags.append(values)
    parts=[]
    for kind in range(4):
        size=reader.words(1)[0]
        if kind==1 and not textured:
            if size:raise ValueError('Untextured procedural UV data')
        elif not 64<=size<=8192 or size%16:raise ValueError('Procedural packet extent')
        parts.append(reader.take(size))
    pos,uv,color,index=parts
    positions=[v for c in unpack_records(pos,0x79,'<3h') for v in c]
    colors=[v for c in unpack_records(color,0x6e,'<4B') for v in c]
    coords=[v for c in unpack_records(uv,0x75,'<2H') for v in c] if textured else [(0,0)]
    if len(tags[0])!=len(positions) or len(tags[1])!=(len(coords) if textured else 0):
        raise ValueError('Procedural tag/geometry extent')
    vertices=[]
    for strip in unpack_records(index,0x6e,'<4B'):
        if len(strip)<3:raise ValueError('Procedural strip extent')
        corners=[]
        for p,n,c,t in strip:
            if n or p>=len(positions) or c>=len(colors) or t>=len(coords):raise ValueError('Procedural corner extent')
            corners.append((*[x/64 for x in positions[p]],1.,*colors[c],coords[t][0]/32768,coords[t][1]/32768,0.,0.,tags[0][p],tags[1][t] if textured else 0))
        for i in range(len(corners)-2):vertices.extend(corners[i:i+3])
    return vertices

def triangle_set(v):return Counter(tuple(sorted(v[i:i+3])) for i in range(0,len(v),3))

def compare(original,native):
    r=Reader(original);n=Reader(native);results=[]
    for case,(shape,seed) in enumerate(CASES):
        header=r.words(5);other=n.words(5)
        if header[:3]!=(case,shape,seed) or other[:4]!=header[:4]:raise ValueError('Procedural identity/RNG mismatch')
        if not 1<=header[4]<=32 or other[4]!=COUNTS[shape]:raise ValueError('Procedural piece/vertex extent')
        expected=[]
        for _ in range(header[4]):expected.extend(piece(r,shape in (56,62)))
        actual=list(struct.iter_unpack('<12f2I',n.take(other[4]*56)))
        if expected!=actual:
            missing=triangle_set(expected)-triangle_set(actual);extra=triangle_set(actual)-triangle_set(expected)
            raise ValueError(f'Procedural case{case}/shape{shape} geometry mismatch: missing{list(missing.items())[:1]}, extra{list(extra.items())[:1]}')
        if shape==45 and header[3]!=seed:raise ValueError('Nonrandom rays consumed RNG')
        results.append(dict(case=case,shape=shape,seed=seed,final_rng=header[3],vertices=len(actual),ordered=expected==actual))
    r.finish();n.finish();return results

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--oracle',type=Path,default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference',type=Path,default=Path('local/research/procedural-effects-v1'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);a.reference.mkdir(parents=True,exist_ok=True)
    b=a.reference;elf=Path('local/extracted/SCUS_971.29');ram=Path('local/research/shadow-transition-v1/before-shadow.ram')
    if digest(elf)!=ELF_SHA:raise ValueError('Different retail ELF')
    inp=b/'shapes-fixture-v1.bin';write_exact(inp,fixture())
    original=b/'shapes-original-v1.bin';manifest=b/'shapes-original-v1.run.json'
    if not original.exists():
        if manifest.exists():raise ValueError('Retained failed original attempt; investigate before using fresh reference output')
        cmd=[str(a.oracle),'procedural-effect-shape',str(elf),str(ram),str(inp),str(original)]
        t=time.monotonic()
        with (b/'shapes-original-v1.log').open('w') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
        run=dict(command=cmd,exit_code=r.returncode,elapsed_seconds=time.monotonic()-t,binary_sha256=digest(a.oracle),elf_sha256=ELF_SHA,input_sha256=digest(inp),ram_sha256=digest(ram))
        if not r.returncode:run['output_sha256']=digest(original)
        write_exact(manifest,(json.dumps(run,indent=2)+'\n').encode())
        if r.returncode:raise RuntimeError('Original procedural shape probe failed')
    run=json.loads(manifest.read_text())
    if run['exit_code'] or run['elf_sha256']!=ELF_SHA or any(run[k]!=digest(path) for k,path in [('input_sha256',inp),('ram_sha256',ram),('output_sha256',original)]):
        raise ValueError('Changed/incomplete original procedural corpus')
    textures=Path('local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF')
    out=a.output/'native.bin';cmd=[str(a.native),'procedural-effect-shape',str(inp),str(textures),str(out)]
    subprocess.run(cmd,check=True)
    cases=compare(original.read_bytes(),out.read_bytes())
    report=dict(scope=__doc__,original_run=run,native_command=cmd,native_sha256=digest(a.native),cases=cases,vertices_exact=sum(c['vertices'] for c in cases),texture_sha256={name:digest(textures/name) for name in ('kyusyu16.xim','oldskilt4.xim')})
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(f"{len(cases)} cases/{report['vertices_exact']} vertices with tags/UVs and final RNG exact")

if __name__=='__main__':main()
