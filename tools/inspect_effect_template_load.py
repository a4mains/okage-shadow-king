#!/usr/bin/env python3
"""Audit a bounded original noise initializer + complete owned EFD import.

This verifies observed RNG continuity and records cache/template metadata. It
does not compare a native loader or establish the game's cold-start seed/state.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
from verify_event import write_exact
from verify_map_effect import digest, ELF_SHA
from verify_shadow_drawing import Reader

CALLERS={0x10e0e0:'noise-grid',0x1119e4:'surface',0x111a10:'surface',
         0x10dd38:'rays',0x10dd6c:'rays',0x10dd98:'rays',0x10ddc4:'rays',0x10dff0:'rays',
         0x10ea10:'spray',0x10eaa0:'spray',0x10eb10:'spray',0x10eb80:'spray'}

def inspect(data,seed,efd_count):
    r=Reader(data);magic,got,grid_rng,final_rng,count,calls=r.words(6)
    if magic!=0x504d5445 or got!=seed or count!=efd_count or not 384<=calls<=10000:
        raise ValueError('Template loader identity/extent')
    grid=r.words(384);state=seed;sites=Counter();runs=[]
    for i in range(calls):
        ra,observed=r.words(2);site=(ra-8)&0xffffffff
        if observed!=state:raise ValueError('Discontinuous original RNG observations')
        if site not in CALLERS:raise ValueError(f'Unreviewed RNG caller {site:x}')
        kind=CALLERS[site];sites[f'0x{site:08x}']+=1
        if not runs or runs[-1]['constructor']!=kind:
            runs.append(dict(constructor=kind,first_call=i,initial_state=state,calls=0))
        runs[-1]['calls']+=1
        state=(state*0x41c64e6d+0x3039)&0xffffffff
        if i<384 and (site!=0x10e0e0 or grid[i]!=(state&0x7fffffff)%7):
            raise ValueError('Original noise grid differs from observed RNG')
        if i==383 and state!=grid_rng:raise ValueError('Original grid RNG endpoint')
        runs[-1]['final_state']=state
    if state!=final_rng:raise ValueError('Original loader RNG endpoint')
    cache=[]
    for shape in range(66):
        owner,element=r.words(2)
        if owner>=128:raise ValueError('Original cache owner extent')
        cache.append(dict(shape=shape,owner=owner,element=element))
    templates=[]
    for lookup in range(128):
        identity,runtime_id,flags,elements=r.words(4)
        if identity!=lookup or (runtime_id!=0xffffffff and (runtime_id>=128 or elements>4096)):
            raise ValueError('Original template observation extent')
        if runtime_id!=0xffffffff:templates.append(dict(lookup=lookup,runtime_id=runtime_id,flags=flags,elements=elements))
    if len(templates)!=count:raise ValueError('Missing imported template observations')
    r.finish()
    return dict(seed=seed,template_count=count,rng_calls=calls,grid_rng_state=grid_rng,
                final_rng_state=final_rng,call_sites=dict(sites),constructor_runs=runs,
                shape_cache=cache,templates=templates)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('manifest',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();run=json.loads(a.manifest.read_text());cmd=run['command']
    if run['exit_code'] or run['elf_sha256']!=ELF_SHA or len(cmd)!=7 or cmd[1]!='effect-template-load':
        raise ValueError('Incomplete/unrecognized original loader run')
    for key,index in (('elf_sha256',2),('ram_sha256',3),('efd_sha256',4),('output_sha256',6)):
        if digest(Path(cmd[index]))!=run[key]:raise ValueError('Changed original loader input/output')
    efd=Path(cmd[4]).read_bytes()
    if len(efd)<16 or efd[:4]!=b'EFD ':raise ValueError('Owned EFD header')
    seed=int(cmd[5],0)
    if seed!=run['seed']:raise ValueError('Loader seed provenance')
    result=inspect(Path(cmd[6]).read_bytes(),seed,struct.unpack_from('<I',efd,4)[0])
    write_exact(a.output,(json.dumps(dict(scope=__doc__,original_run=run,**result),indent=2)+'\n').encode())
    print(f"{result['template_count']} original templates; {result['rng_calls']} continuous observed RNG calls")

if __name__=='__main__':main()
