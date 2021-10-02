#!/usr/bin/env python3
"""Compare assembled owned shadow geometry using explicit world/view/light/screen settings.

Original binding, rest pose and view/world composition, batch selection, skinning,
projection setup and VU1 cache/draw stages execute unchanged. The observer drives
VU entries directly; it does not execute the complete packet compiler or GS raster.
No captured RAM, poses or vertices feed the native implementation.
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
    identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];rows=[]
    for case in range(6):
        view=identity.copy();world=identity.copy();world[14]=300
        mode,factor=[(4,1),(4,.5),(5,1),(6,1),(7,1),(4,0)][case]
        settings=[800,1,.47,2048,2048,101,16777115,3,2048,3,1024,0,0,320,112]
        if case==1:
            view[:8]=[.8660254,.5,0,0,-.5,.8660254,0,0];view[13]=-20
            world[12]=30;settings[11:13]=[50,400]
        if case==2:
            world[:12]=[0,0,-1.25,0,0,.75,0,0,1.5,0,0,0];world[12]=50;world[14]=80
        if case==3:world[14]=2000
        if case==4:world[14]=5
        if case==5:world[12]=-1000
        rows.append(struct.pack('<2I53f3I',case,mode,0,factor,.3,1,.125,0,*view,*world,*settings,255,64,192))
    return b''.join(rows)

def compare(original,native,inputs,nodes,triangles):
    if not inputs or len(inputs)%232 or len(inputs)>32*232:raise ValueError('Shadow assembly fixture extent')
    if not original or len(original)%4 or len(native)!=len(original):raise ValueError('Shadow assembly output extent')
    a=struct.unpack('<%dI'%(len(original)//4),original);b=struct.unpack('<%dI'%(len(native)//4),native)
    if a!=b:
        differences=[(i,hex(x),hex(y)) for i,(x,y) in enumerate(zip(a,b)) if x!=y]
        raise ValueError(f'Shadow assembly differs: {differences[:8]}')
    at=emitted=0;counts=[]
    for case in range(len(inputs)//232):
        identity,mode=struct.unpack_from('<2I',inputs,case*232)
        if identity!=case or not 4<=mode<=7:raise ValueError('Shadow assembly fixture identity')
        if len(a)-at<3 or a[at:at+3]!=(case,nodes,triangles):raise ValueError('Shadow assembly record header')
        at+=3+nodes*16;last=None;drawn=0
        for _ in range(triangles):
            if len(a)-at<4:raise ValueError('Truncated shadow submission')
            block,batch,triangle,draw=a[at:at+4];at+=4
            if block or draw not in (0,1):raise ValueError('Shadow submission identity')
            if last is None:
                if (batch,triangle)!=(0,0):raise ValueError('Shadow submission first triangle')
            elif (batch,triangle) not in ((last[0],last[1]+1),(last[0]+1,0)):
                raise ValueError('Shadow submission ordering')
            last=(batch,triangle);drawn+=draw;at+=draw*144
            if at>len(a):raise ValueError('Truncated shadow geometry')
        counts.append(drawn);emitted+=drawn
    if at!=len(a):raise ValueError('Shadow assembly trailing data')
    return dict(cases=len(counts),matrices_exact=nodes*len(counts),triangles_checked=triangles*len(counts),
                emitted_prisms=emitted,ordered_vertices_exact=emitted*36,emitted_per_case=counts)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--oracle',type=Path,default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference',type=Path,default=Path('local/research/shadow-assembled-v1'))
    p.add_argument('--assets',type=Path,default=Path('local/xpf-decoded/FLDDATA/OBJ'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);a.reference.mkdir(parents=True,exist_ok=True)
    inputs=a.reference/'fixture-v1.bin';write_exact(inputs,fixture())
    elf=Path('local/extracted/SCUS_971.29')
    if digest(elf)!=ELF_SHA:raise ValueError('Different owned ELF')
    cases=[]
    for name,nodes,triangles in [('HERO',112,302),('MAM',79,311),('IMO',85,394),('PAP',70,392)]:
        root=a.assets/(name+'.XPF');sources=[root/'a.xmd',root/'shadow.xmd',inputs]
        original=a.reference/(name+'-original-v1.bin');manifest=original.with_suffix('.run.json')
        hashes={p.name:digest(p) for p in sources}
        if not original.exists():
            if manifest.exists():raise ValueError('Retained original failure; investigate before retry')
            cmd=[str(a.oracle),'shadow-assembled',str(elf),*[str(p) for p in sources],str(original)]
            start=time.monotonic()
            with original.with_suffix('.log').open('w') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
            run=dict(command=cmd,exit_code=r.returncode,elapsed_seconds=time.monotonic()-start,
                     binary_sha256=digest(a.oracle),elf_sha256=ELF_SHA,input_sha256=hashes)
            if not r.returncode:run['output_sha256']=digest(original)
            write_exact(manifest,(json.dumps(run,indent=2)+'\n').encode())
            if r.returncode:raise RuntimeError('Original shadow assembly failed')
        run=json.loads(manifest.read_text())
        if run['exit_code'] or run['elf_sha256']!=ELF_SHA or run['input_sha256']!=hashes or run['output_sha256']!=digest(original):
            raise ValueError('Changed/incomplete original shadow assembly')
        native=a.output/(name+'-native.bin');cmd=[str(a.native),'shadow-assembled',*[str(p) for p in sources],str(native)]
        subprocess.run(cmd,check=True)
        cases.append(dict(name=name,original_run=run,native_command=cmd,**compare(original.read_bytes(),native.read_bytes(),inputs.read_bytes(),nodes,triangles)))
    report=dict(scope=__doc__,native_sha256=digest(a.native),cases=cases)
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps({c['name']:c['emitted_per_case'] for c in cases}))

if __name__=='__main__':main()
