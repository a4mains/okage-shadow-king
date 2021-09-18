#!/usr/bin/env python3
"""Verify native procedural preparation against complete original EFD import.

Checks the noise grid, every RNG input state, final state, all66 cache records,
plus ordered geometry/colors/UVs/tags of43/45/55/56/62 after template sharing.
Other deterministic geometry and runtime programs are outside this comparison.
This explicit seed1/loaded-texture fixture does not prove cold-start fidelity.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from inspect_effect_template_load import inspect
from verify_map_effect import digest,ELF_SHA
from verify_event import write_exact
from verify_shadow_drawing import Reader
from verify_procedural_effects import piece,COUNTS

def compare(original,geometry,native):
    r=Reader(original);n=Reader(native);g=Reader(geometry)
    magic,seed,grid_rng,final_rng,count,calls=r.words(6)
    if magic!=0x504d5445 or not 384<=calls<=10000 or count>128:raise ValueError('Prepared fixture extent')
    inspect(original,seed,count)
    if n.words(4)!=(seed,final_rng,calls,count):raise ValueError('Prepared identity/RNG/count')
    if n.words(384)!=r.words(384):raise ValueError('Prepared noise grid')
    states=[]
    for _ in range(calls):ra,state=r.words(2);states.append(state)
    if n.words(calls)!=tuple(states):raise ValueError('Prepared RNG sequence')
    cache=r.words(132)
    if n.words(132)!=cache:raise ValueError('Prepared cache owners')
    r.words(128*4);r.finish();total=0
    for shape in (43,45,55,56,62):
        got,owner,element,pieces=g.words(4)
        if (got,owner,element)!=(shape,*cache[shape*2:shape*2+2]) or not 1<=pieces<=32:
            raise ValueError('Prepared geometry cache identity')
        expected=[]
        for _ in range(pieces):expected.extend(piece(g,shape in (56,62)))
        if n.words(2)!=(shape,COUNTS[shape]):raise ValueError('Prepared geometry extent')
        actual=list(struct.iter_unpack('<12f2I',n.take(COUNTS[shape]*56)))
        if actual!=expected:raise ValueError(f'Prepared shape{shape} ordered vertices/tags differ')
        total+=len(actual)
    g.finish();n.finish()
    return dict(grid_words_exact=384,rng_states_exact=calls,cache_records_exact=66,ordered_vertices_exact=total)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--reference',type=Path,default=Path('local/research/procedural-effects-v1'))
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);b=a.reference
    run=json.loads((b/'loader-geometry-original-v1.run.json').read_text());cmd=run['command']
    if run['exit_code'] or run['elf_sha256']!=ELF_SHA or len(cmd)!=7 or cmd[1]!='effect-template-geometry':
        raise ValueError('Incomplete original preparation corpus')
    for key,index in (('elf_sha256',2),('ram_sha256',3),('efd_sha256',4)):
        if digest(Path(cmd[index]))!=run[key]:raise ValueError('Changed preparation input')
    for name,sha in run['outputs'].items():
        if digest(b/name)!=sha:raise ValueError('Changed preparation output')
    old=Path('local/research/effect-template-v1/loader-seed1-original-v1.bin')
    original=b/'loader-geometry-original-v1.bin'
    if original.read_bytes()!=old.read_bytes():raise ValueError('Original loader state changed with added geometry observer')
    out=a.output/'native.bin';textures=Path('local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF')
    native_cmd=[str(a.native),'prepare-effect-templates',cmd[4],str(textures),cmd[5],str(out)]
    subprocess.run(native_cmd,check=True)
    result=compare(original.read_bytes(),Path(str(original)+'.geometry.bin').read_bytes(),out.read_bytes())
    report=dict(scope=__doc__,original_run=run,native_command=native_cmd,native_sha256=digest(a.native),**result)
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(f"{result['rng_states_exact']} RNG states, 66 cache records and {result['ordered_vertices_exact']} ordered vertices/tags exact")

if __name__=='__main__':main()
