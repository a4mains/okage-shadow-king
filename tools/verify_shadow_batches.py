#!/usr/bin/env python3
"""Compare native shadow batches with original mode1 source polygon selection.

Original0x17ac20/0x17ac90/0x17b9d8 run on owned XMD inputs. Compare both
material passes, every batch boundary, source position map and remapped corner.
This does not execute skinning, the packet compiler or rasterization.
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

def compare(original,native):
    if not original or len(original)%4 or original!=native:
        raise ValueError('Shadow batch word/extent mismatch')
    words=struct.unpack('<%dI'%(len(original)//4),original);at=0
    def take():
        nonlocal at
        if at>=len(words):raise ValueError('Truncated shadow batch')
        v=words[at];at+=1;return v
    blocks=take()
    if not 0<blocks<=4096:raise ValueError('Shadow block count')
    batches=positions=polygons=corners=0
    for _ in range(blocks*2):
        count=take()
        if count>4096:raise ValueError('Shadow batch count')
        batches+=count
        for _ in range(count):
            n,p=take(),take()
            if not 0<n<=97 or not 0<p<=97:raise ValueError('Shadow batch capacity')
            mapping=[take() for _ in range(n)]
            if len(set(mapping))!=n or max(mapping)>=32768:raise ValueError('Shadow source map')
            positions+=n;polygons+=p;seen=set()
            for _ in range(p):
                k=take()
                if k not in (3,4):raise ValueError('Shadow polygon size')
                corners+=k
                for _ in range(k):
                    i=take()
                    if i>=n:raise ValueError('Shadow corner index')
                    if i not in seen:
                        if i!=len(seen):raise ValueError('Shadow first-use order')
                        seen.add(i)
            if len(seen)!=n:raise ValueError('Unused shadow position')
    if at!=len(words):raise ValueError('Shadow batch trailing words')
    return dict(blocks=blocks,batches=batches,positions_exact=positions,
                polygons_exact=polygons,corners_exact=corners)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--oracle',type=Path,default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference',type=Path,default=Path('local/research/shadow-batches-v1'))
    p.add_argument('--assets',type=Path,default=Path('local/xpf-decoded/FLDDATA/OBJ'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    a.reference.mkdir(parents=True,exist_ok=True)
    elf=Path('local/extracted/SCUS_971.29')
    if digest(elf)!=ELF_SHA:raise ValueError('Different owned ELF')
    cases=[]
    for name in ['HERO','MAM','IMO','PAP']:
        source=a.assets/(name+'.XPF')/'shadow.xmd'
        original=a.reference/(name+'-original-v1.bin');manifest=original.with_suffix('.run.json')
        if not original.exists():
            if manifest.exists():raise ValueError('Retained original failure; investigate before retry')
            cmd=[str(a.oracle),'shadow-batches',str(elf),str(source),str(original)]
            start=time.monotonic()
            with original.with_suffix('.log').open('w') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
            run=dict(command=cmd,exit_code=r.returncode,elapsed_seconds=time.monotonic()-start,
                     binary_sha256=digest(a.oracle),elf_sha256=ELF_SHA,input_sha256=digest(source))
            if not r.returncode:run['output_sha256']=digest(original)
            write_exact(manifest,(json.dumps(run,indent=2)+'\n').encode())
            if r.returncode:raise RuntimeError('Original shadow batching failed')
        run=json.loads(manifest.read_text())
        if run['exit_code'] or run['elf_sha256']!=ELF_SHA or run['input_sha256']!=digest(source) or run['output_sha256']!=digest(original):
            raise ValueError('Changed/incomplete original shadow batches')
        native=a.output/(name+'-native.bin');cmd=[str(a.native),'shadow-batches',str(source),str(native)]
        subprocess.run(cmd,check=True)
        cases.append(dict(name=name,original_run=run,native_command=cmd,**compare(original.read_bytes(),native.read_bytes())))
    report=dict(scope=__doc__,native_sha256=digest(a.native),cases=cases)
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps({c['name']:{k:v for k,v in c.items() if k.endswith('_exact') or k=='batches'} for c in cases}))

if __name__=='__main__':main()
