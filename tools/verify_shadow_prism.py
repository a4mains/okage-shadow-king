#!/usr/bin/env python3
"""Compare native three-pass shadow prism geometry with original VU1 packets.

Explicit synthetic camera/screen caches isolate draw selection and packet
geometry. Original microcode and XGKICK execute with a null graphics sink.
This does not validate cache production, shadow model skinning, pixels or field
integration. Native output contains geometry, never guest instructions.
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

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def fixture():
    lower = [(0,0,3,0),(1,0,3,0),(0,0,4,0)]
    upper = [(x,y+1,z,flags) for x,y,z,flags in lower]
    screen = [(1024,1024,10000,128),(1280,1024,10000,128),(1024,1280,10000,128)]
    other = [(x,y-100,z+50,w) for x,y,z,w in screen]
    rows = []
    rng = random.Random(219700)
    for case in range(128):
        lo,up,sc,ot = [list(v) for v in (lower,upper,screen,other)]
        direction = (0,1,0,0)
        status = 0
        if case==1:direction=(0,-1,0,0)
        if case==2:direction=(1,0,0,0)
        if case==3:
            lo,up,sc,ot = [[v[i] for i in (0,2,1)] for v in (lo,up,sc,ot)]
        if case==4:lo[0]=(*lo[0][:3],1)
        if case==5:
            lo=[(*v[:3],1) for v in lo];up=[(*v[:3],1) for v in up]
        if case==6:up[2]=(*up[2][:3],16)
        if case==7:sc[0]=(0,*sc[0][1:])
        if case==8:sc[0]=(4096,*sc[0][1:])
        if case==9:lo[0]=(*lo[0][:3],1);sc[0]=(0,*sc[0][1:])
        if case==10:lo[0]=(*lo[0][:3],1);sc[0]=(4096,*sc[0][1:])
        if case==11:sc=[(x+.0625,y+.03125,z+.123,w+.0625) for x,y,z,w in sc]
        if 12<=case<24:
            lo[0]=(*lo[0][:3],1)
            boundary=[16,16.0001,15.9999,4080,4080.0001,4079.9999][(case-12)%6]
            sc[0]=((boundary,sc[0][1]) if case<18 else (sc[0][0],boundary))+sc[0][2:]
        if case==24:status=-1
        if case==25:status=1
        if case==26:lo=[(0,0,0,0)]*3
        if case==27:up=[(*v[:3],32) for v in up]
        if case>=28:
            lo=[tuple(rng.uniform(-100,100) for _ in range(3))+(0,) for _ in range(3)]
            up=[tuple(rng.uniform(-100,100) for _ in range(3))+(0,) for _ in range(3)]
            direction=tuple(rng.uniform(-1,1) for _ in range(3))+(0,)
            sc=[(rng.uniform(100,3900),rng.uniform(100,3900),rng.uniform(1000,100000),rng.uniform(0,255)) for _ in range(3)]
            ot=[(rng.uniform(100,3900),rng.uniform(100,3900),rng.uniform(1000,100000),rng.uniform(0,255)) for _ in range(3)]
            if case%3==0:lo[0]=(*lo[0][:3],2)
        row=struct.pack('<Ii4f',case,status,*direction)
        row+=b''.join(struct.pack('<3fI',*v) for v in lo+up)
        row+=b''.join(struct.pack('<4f',*v) for v in sc+ot)
        rows.append(row)
    return b''.join(rows)

TAGS = {0:(1,0xd0024000,0x4444444e,0x00044444),
        1:(1,0,0x4a,0),14:(1,0xf0224000,0x44444eee,0x04444444),
        15:(0,0,0x4a,0),16:(0,0,0x4c,0),17:(0x5400c,0,0x47,0),
        30:(1,0xe0024000,0x444444ee,0x00444444),
        31:(0,0x7fffffff,0x4c,0),32:(0x5000c,0,0x47,0),
        45:(0x4b008000,0xd0024000,0x4444444e,0x00044444)}

def compare(original, native, inputs):
    if not inputs or len(inputs)%216 or len(inputs)>128*216:
        raise ValueError('Shadow prism fixture extent')
    fixtures=list(struct.iter_unpack('<54I', inputs))
    a=b=emitted=0
    for case,f in enumerate(fixtures):
        if f[0]!=case or f[1] not in (0,1,0xffffffff):
            raise ValueError('Shadow prism fixture identity/status')
        values=struct.unpack('<54f',struct.pack('<54I',*f))
        if any(f[9+4*i]>63 for i in range(6)) or not all(math.isfinite(values[i]) for i in range(2,54) if i not in range(9,30,4)):
            raise ValueError('Shadow prism fixture values')
        if len(original)-a<8 or len(native)-b<8:
            raise ValueError('Shadow prism record extent')
        i,qwords=struct.unpack_from('<2I',original,a);a+=8
        j,draw=struct.unpack_from('<2I',native,b);b+=8
        if i!=case or j!=case or qwords not in (0,1,46) or draw not in (0,1) or draw!=(qwords==46):
            raise ValueError(f'Shadow prism identity/emission case{case}')
        if (qwords==0)!=(f[1]==0xffffffff):
            raise ValueError('Shadow prism clip-status contract')
        if len(original)-a<qwords*16 or len(native)-b<draw*576:
            raise ValueError('Shadow prism geometry extent')
        words=struct.unpack_from('<%dI'%(qwords*4),original,a);a+=qwords*16
        if qwords==1 and words!=TAGS[45]:
            raise ValueError('Shadow prism empty terminal tag')
        if draw:
            for offset,tag in TAGS.items():
                if words[offset*4:offset*4+4]!=tag:
                    raise ValueError('Shadow prism pass/terminal tag')
            expected=words[8:56]+words[72:120]+words[132:180]
            actual=struct.unpack_from('<144I',native,b);b+=576
            if expected!=actual:
                diff=[(i,hex(x),hex(y)) for i,(x,y) in enumerate(zip(expected,actual)) if x!=y]
                raise ValueError(f'Shadow prism case{case} vertex words differ: {diff[:6]}')
            emitted+=1
    if a!=len(original) or b!=len(native):raise ValueError('Shadow prism trailing records')
    return dict(cases=len(fixtures),emitted_prisms=emitted,culled_prisms=len(fixtures)-emitted,
                ordered_vertices_exact=emitted*36,original_pass_headers_checked=emitted*9)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--oracle',type=Path,default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    p.add_argument('--reference',type=Path,default=Path('local/research/shadow-prism-v1'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    root=a.reference;root.mkdir(parents=True,exist_ok=True)
    inp=root/'fixture-v2.bin';write_exact(inp,fixture())
    elf=Path('local/extracted/SCUS_971.29')
    if digest(elf)!=ELF_SHA:raise ValueError('Different retail ELF')
    original=root/'original-v4.bin';manifest=root/'original-v4.run.json'
    if not original.exists():
        if manifest.exists():raise ValueError('Retained failed original probe; investigate first')
        cmd=[str(a.oracle),'shadow-prism',str(elf),str(inp),str(original)]
        start=time.monotonic()
        with (root/'original-v4.log').open('w') as log:
            result=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
        run=dict(command=cmd,exit_code=result.returncode,elapsed_seconds=time.monotonic()-start,
                 binary_sha256=digest(a.oracle),elf_sha256=ELF_SHA,input_sha256=digest(inp))
        if result.returncode==0:run['output_sha256']=digest(original)
        write_exact(manifest,(json.dumps(run,indent=2)+'\n').encode())
        if result.returncode:raise RuntimeError('Original shadow prism probe failed')
    run=json.loads(manifest.read_text())
    if run['exit_code'] or run['elf_sha256']!=ELF_SHA or run['input_sha256']!=digest(inp) or run['output_sha256']!=digest(original):
        raise ValueError('Changed/incomplete original shadow prism probe')
    native=a.output/'native.bin'
    cmd=[str(a.native),'shadow-prism',str(inp),str(native)];subprocess.run(cmd,check=True)
    result=compare(original.read_bytes(),native.read_bytes(),inp.read_bytes())
    report=dict(scope=__doc__,original_run=run,native_command=cmd,native_sha256=digest(a.native),**result)
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode());print(json.dumps(result))

if __name__=='__main__':main()
