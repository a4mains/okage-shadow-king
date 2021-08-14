#!/usr/bin/env python3
"""Audit EFD reference headers, template98 smoke and template16 flame state.

Reference57 retains an explicitly unresolved composite element-count mismatch.
No deferred scheduling/drawing, interior loading or whole-scene RNG claim.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_map_effect import compare, digest, ELF_SHA
from verify_torch_effect import compare_quads

REFERENCES = (113,107,98,97,74,73,72,71,70,69,68,67,64,63,61,60,59,57,44,43,42,31)
HEADER_SHA = 'd22b6b9443cfdb1caf88bf3d5bad45469d9f138b782f6698f79cf352d61ed23b'
CASES = (
    ('98','local/effect-reference-v1',19,9,700,
     '67c767e8c957b3d91a3f7531233b824f74f180949c07512543fd7d3b775e75ca',None),
    ('flame16','local/flame16-reference-v1',16,3,10000,
     'd01f156156e1de3b3b15c0eefaa8d33ffb94fbbeb476605ea24286928c9bd271',
     '8f116c812d254b111538e102fdb6851137b5dc8b6d4087ddd668d08230ed6e92'),
    ('flame16-offset','local/flame16-reference-v1',16,3,10000,
     'fba69028f18a841eea4d7373583d71be6718b70c055d0389b954a9fc95759333',
     '673bf40c8bbba82c05bafcaf0cd66e9b1cbe0f5137dce8c7f88cf1da7abc66ce'),
)

def compare_headers(original,native):
    if len(original)!=22*25*4 or len(native)!=len(original):
        raise ValueError('Reference header extent')
    for lookup,ref,got in zip(REFERENCES,struct.iter_unpack('<25I',original),struct.iter_unpack('<25I',native)):
        if ref[0]!=lookup or got[0]!=lookup:
            raise ValueError('Reference lookup identity/order')
        for i,(a,b) in enumerate(zip(ref,got)):
            if lookup==57 and i==4:
                if (a,b)!=(31,13):raise ValueError('Composite expansion evidence changed')
            elif a!=b:raise ValueError(f'Reference{lookup} header word{i} differs')
    return {'words_exact':549,'full_header_records_exact':21,
            'unimplemented_composite_expansion':{'lookup':57,'source':47,
                'original_runtime_elements':31,'native_structural_elements':13}}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,default=Path('build/native-release/okage-assets'))
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    if digest(Path('local/extracted/SCUS_971.29'))!=ELF_SHA:raise ValueError('Different retail ELF')
    efd='local/effect-input/CMNDATA/2D/TES.EFD';textures='local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF'
    def run(name,command):
        result=subprocess.run(command,capture_output=True)
        write_exact(args.output/(name+'.log'),result.stdout+result.stderr)
        record={'command':command,'exit_code':result.returncode,'binary_sha256':digest(args.native)}
        write_exact(args.output/(name+'.run.json'),(json.dumps(record,indent=2)+'\n').encode())
        result.check_returncode();return record
    original=Path('local/effect-reference-v1/headers.original.bin')
    manifest=json.loads(original.with_name('headers.run.json').read_text())
    if manifest['exit_code'] or digest(original)!=HEADER_SHA:raise ValueError('Unreviewed original reference headers')
    output=args.output/'headers.native.bin'
    command=[str(args.native),'effect-reference-probe',efd,str(output)]
    native_run=run('headers',command)
    report={'scope':__doc__,'elf_sha256':ELF_SHA,'efd_sha256':digest(Path(efd)),
            'headers':compare_headers(original.read_bytes(),output.read_bytes()),
            'headers_original_run':manifest,'headers_native_run':native_run,'cases':[]}
    for name,directory,runtime_id,elements,ticks,state_hash,geometry_hash in CASES:
        root=Path(directory);source=root/(name+'.input.bin');original=root/(name+'.original.bin')
        manifest=json.loads((root/(name+'.run.json')).read_text())
        if manifest['exit_code'] or digest(source)!=manifest['input_sha256'] or digest(original)!=state_hash:
            raise ValueError('Unreviewed or incomplete original interior probe')
        output=args.output/(name+'.native.bin')
        command=[str(args.native),'map-effect-probe',efd,textures,str(source),str(ticks),str(output)]
        native_run=run(name,command)
        a,b=original.read_bytes(),output.read_bytes()
        compare(a,b,runtime_id,elements,ticks)
        if a!=b:raise ValueError('Interior state differs; no float tolerance for these fixtures')
        record={'name':name,'runtime_id':runtime_id,'state_words_exact':len(a)//4,
                'ticks':ticks,'original_run':manifest,'native_run':native_run}
        if geometry_hash:
            geometry=Path(str(original)+'.torch-geometry.bin')
            if digest(geometry)!=geometry_hash:raise ValueError('Unreviewed original flame geometry')
            record['quads']=compare_quads(geometry.read_bytes(),Path(str(output)+'.torch-geometry.bin').read_bytes(),ticks)
        stride=29+elements*44
        phases=[row[6] for row in struct.iter_unpack(f'<{stride}I',a)]
        record['phase_wraps']=sum(b<a for a,b in zip(phases,phases[1:]))
        report['cases'].append(record)
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print('Reference98 and flame16 state exact;549 header words exact; reference57 composite count remains unresolved')

if __name__=='__main__':main()
