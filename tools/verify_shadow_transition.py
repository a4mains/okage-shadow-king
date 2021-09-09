#!/usr/bin/env python3
"""Compare the native HERO/STUN update callback with original0x135100.

Uses synthetic initial states and owned research RAM only in the oracle. This
covers follow, phase boundaries, STUN visibility/scale and shadow selectors,
not event54 initiation, sound/effect24, actor construction or shadow rendering.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import time
from verify_event import write_exact
from verify_map_effect import digest, ELF_SHA

def fixture():
    data = bytearray()
    cases = [(1,1.,0,0),(4,1.,0,0),(2,.975,0,0),(1,.025,0,0),
             (0,.4,1,1),(3,.4,0,0),(5,.025,0,0),(1,0.,0,0),
             (2,1.,0,0),(4,0.,0,0),(5,1.,0,0),(1,1.,0,1),(1,1.,1,0)]
    word = lambda f:struct.unpack('<I',struct.pack('<f',f))[0]
    for case,(phase,factor,frozen,locked) in enumerate(cases):
        selector = 0xffff if phase in (2,3,4,5) else 4
        visible = phase in (2,3,4)
        projection = 0. if phase in (2,3,4,5) else 1.
        data += struct.pack('<10I20f',case,phase,locked,frozen,0xffffffff,word(factor),
            selector,visible,word(projection),48,
            .125*case,-.25,.03125,1., -7.57+case,.125*case,-2.5,1.,
            .7,.6,.5,.4, 10.,20.,30.,1., 2.,3.,4.,1.)
    return bytes(data)

def compare(original,native,data):
    if not data or len(data)%120 or len(data)>32*120:
        raise ValueError('Shadow update fixture extent')
    rows = []
    ids = set()
    for f in struct.iter_unpack('<30I',data):
        if f[0] in ids or f[1]>5 or f[6]>65535 or f[7]>1 or f[9]>128:
            raise ValueError('Shadow update fixture scope')
        ids.add(f[0])
        for step in range(f[9]+1):rows.append((f[0],step))
    if len(original)!=len(rows)*22*4 or len(native)!=len(original):
        raise ValueError('Shadow update observation extent')
    for index,(a,b) in enumerate(zip(struct.iter_unpack('<22I',original),struct.iter_unpack('<22I',native))):
        if a[:2]!=rows[index] or b[:2]!=rows[index]:raise ValueError('Shadow update observation identity')
        if a!=b:
            diff=[(i,hex(x),hex(y)) for i,(x,y) in enumerate(zip(a,b)) if x!=y]
            raise ValueError(f'Shadow update case/step{a[:2]} differences{diff}')
    return len(rows),len(original)//4

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--oracle',type=Path,default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference',type=Path,default=Path('local/research/shadow-transition-v1'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    elf=Path('local/extracted/SCUS_971.29')
    if digest(elf)!=ELF_SHA:raise ValueError('Different retail ELF')
    inp=a.reference/'update-fixture-v1.bin';write_exact(inp,fixture())
    ram=a.reference/'before-shadow.ram';original=a.reference/'update-original-v1.bin'
    manifest=a.reference/'update-original-v1.run.json'
    if not original.exists():
        cmd=[str(a.oracle),'shadow-update',str(elf),str(ram),str(inp),str(original)]
        t=time.monotonic()
        with (a.reference/'update-original-v1.log').open('w') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
        record=dict(command=cmd,exit_code=r.returncode,elapsed_seconds=time.monotonic()-t,binary_sha256=digest(a.oracle),elf_sha256=ELF_SHA,input_sha256=digest(inp),ram_sha256=digest(ram))
        if not r.returncode:record['output_sha256']=digest(original)
        write_exact(manifest,(json.dumps(record,indent=2)+'\n').encode())
        if r.returncode:raise RuntimeError('Original shadow update failed')
    original_run=json.loads(manifest.read_text())
    if original_run['exit_code'] or original_run['elf_sha256']!=ELF_SHA or any(original_run[k]!=digest(path) for k,path in [('input_sha256',inp),('ram_sha256',ram),('output_sha256',original)]):
        raise ValueError('Changed/incomplete original update corpus')
    out=a.output/'native.bin';cmd=[str(a.native),'shadow-update',str(inp),str(out)]
    subprocess.run(cmd,check=True)
    records,words=compare(original.read_bytes(),out.read_bytes(),inp.read_bytes())
    report=dict(scope=__doc__,elf_sha256=ELF_SHA,original_run=original_run,native_command=cmd,native_sha256=digest(a.native),records_exact=records,words_exact=words)
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(f'{records} snapshots/{words} words exact')

if __name__=='__main__':main()
