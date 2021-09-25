#!/usr/bin/env python3
"""Compare owned shadow bone binding and rest matrices with original execution.

Original bone/hierarchy/name loaders and0x1b6b40 share the main bone buffer;
0x184a20 evaluates every shadow hierarchy slot. The probe enables unused slots
for comparison and does not claim runtime visibility flags or animated rendering.
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
    if len(original)<4:raise ValueError('Shadow binding header extent')
    count=struct.unpack_from('<I',original)[0]
    if not 0<count<=4096 or len(original)!=4+count*68 or len(native)!=len(original):
        raise ValueError('Shadow binding matrix extent')
    if native!=original:
        a=struct.unpack('<%dI'%(len(original)//4),original);b=struct.unpack('<%dI'%(len(native)//4),native)
        diff=[(i,hex(x),hex(y)) for i,(x,y) in enumerate(zip(a,b)) if x!=y]
        raise ValueError(f'Shadow binding differs: {diff[:6]}')
    if any(v>=count for v in struct.unpack_from('<%dI'%count,original,4)):
        raise ValueError('Shadow binding index outside fixture')
    return dict(bone_indices_exact=count,rest_matrices_exact=count)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--oracle',type=Path,default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference',type=Path,default=Path('local/research/shadow-binding-v1'))
    p.add_argument('--assets',type=Path,default=Path('local/xpf-decoded/FLDDATA/OBJ'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    root=a.reference;root.mkdir(parents=True,exist_ok=True)
    elf=Path('local/extracted/SCUS_971.29')
    if digest(elf)!=ELF_SHA:raise ValueError('Different retail ELF')
    cases=[]
    for name in ['HERO','MAM','IMO','PAP']:
        main=a.assets/(name+'.XPF')/'a.xmd';shadow=main.with_name('shadow.xmd')
        original=root/(name+'-original-v1.bin');manifest=original.with_suffix('.run.json')
        if not original.exists():
            if manifest.exists():raise ValueError('Retained failed binding probe; investigate first')
            cmd=[str(a.oracle),'shadow-bind',str(elf),str(main),str(shadow),str(original)]
            start=time.monotonic()
            with original.with_suffix('.log').open('w') as log:
                r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
            run=dict(command=cmd,exit_code=r.returncode,elapsed_seconds=time.monotonic()-start,
                     binary_sha256=digest(a.oracle),elf_sha256=ELF_SHA,main_sha256=digest(main),shadow_sha256=digest(shadow))
            if r.returncode==0:run['output_sha256']=digest(original)
            write_exact(manifest,(json.dumps(run,indent=2)+'\n').encode())
            if r.returncode:raise RuntimeError('Original shadow binding failed')
        run=json.loads(manifest.read_text())
        if run['exit_code'] or run['elf_sha256']!=ELF_SHA or run['main_sha256']!=digest(main) or run['shadow_sha256']!=digest(shadow) or run['output_sha256']!=digest(original):
            raise ValueError('Changed/incomplete original shadow binding')
        native=a.output/(name+'-native.bin')
        cmd=[str(a.native),'shadow-bind',str(main),str(shadow),str(native)]
        subprocess.run(cmd,check=True)
        cases.append(dict(name=name,original_run=run,native_command=cmd,**compare(original.read_bytes(),native.read_bytes())))
    report=dict(scope=__doc__,native_sha256=digest(a.native),cases=cases)
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps({c['name']:c['bone_indices_exact'] for c in cases}))

if __name__=='__main__':main()
