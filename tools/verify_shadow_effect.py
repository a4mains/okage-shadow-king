#!/usr/bin/env python3
"""Compare effect24 state and deformed ring heights with original execution.

The original uses actual HERO attachment construction and callback, with a
synthetic actor pose in research RAM. Native inputs are owned assets and that
pose only. This does not establish geometry drawing, raster fidelity, moving
actor integration, sound or service54 completion.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_map_effect import digest, ELF_SHA

def compare_state(original, native):
    if len(original)!=84*381*4 or len(native)!=len(original):
        raise ValueError('Shadow effect observation extent')
    rows=list(struct.iter_unpack('<381I',original))
    for step,r in enumerate(rows):
        flags=0 if step<2 else 0x20 if step<82 else 0x120
        if r[:7]!=(step,0,24,flags,8,80,max(1,min(step,81))):
            raise ValueError('Shadow effect lifetime observation')
    if rows[-1][2:]!=rows[-2][2:]:raise ValueError('Shadow effect terminal mutation')
    if native!=original:raise ValueError('Shadow effect state differs')
    return len(original)//4

def compare_rings(original,native):
    if len(original)%4 or len(native)%4:raise ValueError('Ring word extent')
    source=struct.unpack('<%dI'%(len(original)//4),original)
    target=struct.unpack('<%dI'%(len(native)//4),native)
    expected={};seen=set();at=0
    while at<len(source):
        if len(source)-at<4:raise ValueError('Ring source header extent')
        step,element,piece,count=source[at:at+4];at+=4
        key=step,element,piece
        if step>=84 or element>=8 or key in seen or not 0<count<=512 or len(source)-at<count*3:
            raise ValueError('Ring source identity/extent')
        seen.add(key);vertices=source[at:at+count*3];at+=count*3
        if element<3:
            if piece or count!=60:raise ValueError('Ring source shape')
            values=[(vertices[j]>>16)&65535 for j in range(0,len(vertices),3) if vertices[j+2]==2]
            if len(values)!=30:raise ValueError('Ring source tag count')
            expected[step,element]=values
    if len(expected)!=84*3:raise ValueError('Ring source coverage')
    at=0;entries=0
    while at<len(target):
        if len(target)-at<3:raise ValueError('Ring native header extent')
        step,element,count=target[at:at+3];at+=3
        if count!=30 or len(target)-at<count:raise ValueError('Ring native shape')
        if list(target[at:at+count])!=expected.pop((step,element),None):raise ValueError('Ring native values/identity')
        at+=count;entries+=count
    if expected:raise ValueError('Missing native ring observations')
    return entries

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--reference',type=Path,default=Path('local/research/effect24-v1'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    ref=a.reference;run=json.loads((ref/'24.original-v1.run.json').read_text())
    if run['exit_code'] or run['elf_sha256']!=ELF_SHA or digest(Path('local/extracted/SCUS_971.29'))!=ELF_SHA:
        raise ValueError('Incomplete/different original execution')
    if digest(ref/'24.input.bin')!=run['input_sha256']:raise ValueError('Different original input')
    for name,sha in run['outputs'].items():
        if digest(ref/name)!=sha:raise ValueError('Different original output: '+name)
    output=a.output/'state.bin'
    cmd=[str(a.native),'map-effect-probe','local/effect-input/CMNDATA/2D/TES.EFD',
         'local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF',str(ref/'24.input.bin'),'82',str(output)]
    subprocess.run(cmd,check=True)
    words=compare_state((ref/'24.original-v1.bin').read_bytes(),output.read_bytes())
    entries=compare_rings((ref/'24.original-v1.bin.geometry.bin').read_bytes(),Path(str(output)+'.deformation.bin').read_bytes())
    report=dict(scope=__doc__,original_run=run,native_command=cmd,native_sha256=digest(a.native),
                state_words_exact=words,ring_heights_exact=entries)
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(f'{words} state words/{entries} ring heights exact')

if __name__=='__main__':main()
