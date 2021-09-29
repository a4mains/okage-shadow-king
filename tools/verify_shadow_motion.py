#!/usr/bin/env python3
"""Compare bound animated shadow matrices and source positions against original execution.

Original motion, bone binding/pose and shadow VU1 entries0/4/12/14 run on owned
XMD/XMT assets. Skinning observes source positions in97-position windows, not
compiled draw batches. No raster, scene submission or gameplay fidelity claim.
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

def compare(original,native,frames):
    if len(original)<12 or len(original)%4 or len(native)!=len(original):raise ValueError('Shadow motion extent')
    a=struct.unpack('<%dI'%(len(original)//4),original)
    b=struct.unpack('<%dI'%(len(native)//4),native)
    if a!=b:
        differences=[(i,hex(x),hex(y)) for i,(x,y) in enumerate(zip(a,b)) if x!=y]
        raise ValueError(f'Shadow motion differs: {differences[:8]}')
    nodes,blocks,textures=a[:3]
    if not 0<nodes<=4096 or not 0<blocks<=4096 or textures>240 or len(a)<3+blocks or not 0<=frames<=180:
        raise ValueError('Shadow motion header')
    extents=a[3:3+blocks]
    if any(n>32768 for n in extents):raise ValueError('Shadow position extent')
    stride=3+16*nodes+4*sum(extents)+textures;header=3+blocks
    if len(a)!=header+stride*(frames+1):raise ValueError('Shadow motion sample extent')
    for frame in range(frames+1):
        if a[header+frame*stride]!=frame:raise ValueError('Shadow frame order')
    return dict(samples=frames+1,matrices_exact=nodes*(frames+1),positions_exact=sum(extents)*(frames+1),
                texture_slots_exact=textures*(frames+1),matrix_count=nodes,block_position_counts=list(extents))

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--oracle',type=Path,default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference',type=Path,default=Path('local/research/shadow-motion-v2'))
    p.add_argument('--assets',type=Path,default=Path('local/xpf-decoded/FLDDATA/OBJ'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);a.reference.mkdir(parents=True,exist_ok=True)
    elf=Path('local/extracted/SCUS_971.29')
    if digest(elf)!=ELF_SHA:raise ValueError('Different owned ELF')
    cases=[];entry=1;frames=60
    for name in ['HERO','MAM','IMO','PAP']:
        root=a.assets/(name+'.XPF');inputs=[root/'a.xmd',root/'shadow.xmd',root/'a.xmt',root/'a.xta']
        original=a.reference/(name+'-original-v1.bin');manifest=original.with_suffix('.run.json')
        hashes={p.name:digest(p) for p in inputs}
        if not original.exists():
            if manifest.exists():raise ValueError('Retained failed probe; investigate before retry')
            cmd=[str(a.oracle),'shadow-motion',str(elf),*[str(p) for p in inputs],str(entry),str(frames),str(original)]
            start=time.monotonic()
            with original.with_suffix('.log').open('w') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
            run=dict(command=cmd,exit_code=r.returncode,elapsed_seconds=time.monotonic()-start,
                     binary_sha256=digest(a.oracle),elf_sha256=ELF_SHA,input_sha256=hashes,entry=entry,frames=frames)
            if not r.returncode:run['output_sha256']=digest(original)
            write_exact(manifest,(json.dumps(run,indent=2)+'\n').encode())
            if r.returncode:raise RuntimeError('Original shadow motion failed')
        run=json.loads(manifest.read_text())
        if run['exit_code'] or run['elf_sha256']!=ELF_SHA or run['input_sha256']!=hashes or run['entry']!=entry or run['frames']!=frames or run['output_sha256']!=digest(original):
            raise ValueError('Changed/incomplete original shadow motion')
        native=a.output/(name+'-native.bin')
        cmd=[str(a.native),'shadow-motion',*[str(p) for p in inputs],str(entry),str(frames),str(native)]
        subprocess.run(cmd,check=True)
        cases.append(dict(name=name,original_run=run,native_command=cmd,**compare(original.read_bytes(),native.read_bytes(),frames)))
    report=dict(scope=__doc__,native_sha256=digest(a.native),cases=cases)
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps({c['name']:{k:v for k,v in c.items() if k.endswith('_exact') or k=='samples'} for c in cases}))

if __name__=='__main__':main()
