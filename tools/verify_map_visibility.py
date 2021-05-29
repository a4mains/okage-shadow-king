#!/usr/bin/env python3
"""Bounded retail visibility callback and owned-map native visibility audit.

Unpatched nonnegative-ID callback cases validate visibility/body flags. Asset
checks validate initial flags, ID routing and hide/show restoration. Neither is
an original map-construction execution or full renderer comparison.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from prepare_assets import ELF_HASH
from verify_event import digest, write_exact

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    elf=Path('local/extracted/SCUS_971.29')
    if digest(elf)!=ELF_HASH:raise ValueError('Wrong ELF')
    records=[(instance,mode,flags,index,0xaaaaaaaa,0xffffffff,0,0x12345679)
             for instance in [0,1,18,2047] for mode in [0x100,0x101,0x102,0x1ff,0x300]
             for flags in [0,0x1900,0xffff] for index in range(4)]
    fixture=a.output/'cases.bin'
    write_exact(fixture,b''.join(struct.pack('<8I',*r) for r in records))
    oracle=Path('build/play-research/okage-oracle/okage-pose-oracle')
    original=a.output/'original.bin'
    command=[str(oracle),'instance-visibility',str(elf),str(fixture),str(original)]
    with (a.output/'original.log').open('w') as log:
        subprocess.run(command,check=True,stdout=log,stderr=subprocess.STDOUT)
    expected=original.read_bytes()
    if len(expected)!=len(records)*24:raise ValueError('Original extent')
    maps=[Path('local/xpf-decoded/FLDDATA')/name for name in [
        'ROOM/HOUSE/HOME1B.XPF/a.xmp','ROOM/HOUSE/HOME1C.XPF/a.xmp',
        'MAP/FIELD/FIELD_1D.XPF/a.xmp']]
    results=[];artifacts=[elf,oracle,fixture,original]+maps
    for build in ['native-release','native','model-sanitize']:
        binary=Path('build')/build/'okage-assets';native=a.output/(build+'.bin')
        native_command=[str(binary),'instance-visibility',str(fixture),str(native)]
        subprocess.run(native_command,check=True)
        if native.read_bytes()!=expected:raise ValueError('Native callback differs: '+build)
        checks=[]
        for game_map in maps:
            target=a.output/(build+'-'+game_map.parent.name+'.bin')
            cmd=[str(binary),'map-visibility',str(game_map),str(target)]
            subprocess.run(cmd,check=True)
            rows=list(struct.iter_unpack('<7I',target.read_bytes()))
            data=game_map.read_bytes();h=struct.unpack_from('<16I',data)
            count=0
            for i in range(h[3]):
                placement=struct.unpack_from('<20I',data,h[2]+i*80)
                if (placement[3]>>20)&15:continue
                definition=struct.unpack_from('<37I',data,h[10]+placement[2]*148)
                row=rows[count];count+=1
                visible=int(not ((placement[3]|definition[7])&0x10000))
                if row[0]!=i or row[2:]!=(visible,0,1,visible,int(bool(definition[7]&8))):
                    raise ValueError('Placement visibility mismatch')
            if count!=len(rows):raise ValueError('Placement extent')
            if game_map.parent.name=='HOME1B.XPF' and rows[-1]!=(7,1,0,0,1,0,0):
                raise ValueError('HOME1B pot identity/initial state changed')
            checks.append(dict(map=str(game_map),placements=count,command=cmd))
            artifacts.append(target)
        modified=bytearray(maps[0].read_bytes());h=struct.unpack_from('<16I',modified)
        struct.pack_into('<I',modified,h[10]+128,3)
        bad=a.output/(build+'-collision.xmp');write_exact(bad,modified)
        proc=subprocess.run([str(binary),'map-visibility',str(bad),str(a.output/(build+'-bad.bin'))],capture_output=True,text=True)
        if proc.returncode==0 or 'Map model collision construction is unrecovered' not in proc.stderr:
            raise ValueError('Missing collision guard')
        results.append(dict(build=build,callback_command=native_command,maps=checks,collision_guard=True))
        artifacts.extend([binary,native,bad])
    report=dict(scope=__doc__,elf_sha256=ELF_HASH,original_command=command,original_exit_code=0,
                patched_original_functions=[],cases=len(records),exact_words=len(records)*6,
                builds=results,sha256={str(f):digest(f) for f in artifacts},
                limits=['Negative alias resolution is outside this isolated fixture.',
                        'Map collision construction is guarded for unrecovered geometry types beyond sphere/capsule.',
                        'Native instance flags are represented by visibility; retail other-bit preservation is asserted in the oracle.',
                        'Native payload sentinel is fixed; field service dispatch is covered separately by field-tests.'])
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print('Exact visibility callback cases:',len(records),'in three builds; owned map checks pass')

if __name__=='__main__':main()
