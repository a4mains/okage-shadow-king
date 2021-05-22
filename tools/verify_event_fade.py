#!/usr/bin/env python3
"""Compare event0x4f color/target/hold/alpha state against original EE execution.

The original overlay-enable boundary is a no-op; its RGBA submission is recorded.
This validates callback arithmetic/state, not complete fade timing or GS blending.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from prepare_assets import ELF_HASH
from verify_event import digest, write_exact
from verify_message_opening import require, rows

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,default=Path('build/native-release/okage-assets'))
    p.add_argument('--reference',type=Path,help='Reuse completed original directory')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--capture',type=Path,help='Audit completed next-night integration and report its camera limitation')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    elf=Path('local/extracted/SCUS_971.29')
    if digest(elf)!=ELF_HASH:raise ValueError('Wrong retail ELF')
    records=[(percent,mode&0xffffffff,0x123456,current)
             for current in [0,0x3f000000,0x3f800000] for percent in [0,1,25,50,75,100] for mode in [0,1,2,-1]]
    fixture=a.output/'cases.bin';write_exact(fixture,b''.join(struct.pack('<4I',*r) for r in records))
    original=a.output/'original.bin';native=a.output/'native.bin'
    oracle=Path('build/play-research/okage-oracle/okage-pose-oracle')
    command=[str(oracle),'scene-fade',str(elf),str(fixture),str(original)]
    if a.reference:
        prior=json.loads((a.reference/'report.json').read_text())
        if prior['fixture_sha256']!=digest(fixture) or prior['original_exit_code']!=0:
            raise ValueError('Changed reference cases')
        if digest(a.reference/'original.bin')!=prior['original_sha256']:
            raise ValueError('Changed original result')
        write_exact(original,(a.reference/'original.bin').read_bytes())
        original_hash=prior['oracle_sha256'];command=prior['original_command']
    else:
        with (a.output/'original.log').open('w') as log:
            subprocess.run(command,check=True,stdout=log,stderr=subprocess.STDOUT)
        original_hash=digest(oracle)
    native_command=[str(a.native),'scene-fade',str(fixture),str(native)]
    subprocess.run(native_command,check=True)
    if original.read_bytes()!=native.read_bytes():
        raise ValueError('Event fade state differs')
    if native.stat().st_size!=len(records)*24:raise ValueError('Fade output extent')
    report=dict(scope=__doc__,elf_sha256=ELF_HASH,cases=len(records),exact_words=len(records)*6,
                fixture_sha256=digest(fixture),original_sha256=digest(original),native_sha256=digest(a.native),
                oracle_sha256=original_hash,original_command=command,native_command=native_command,
                original_exit_code=0,boundaries=['0x18fb68 overlay enable no-op','0x18fc10 records RGBA'])
    if a.capture:
        prefix='local/reference-next-night-600-v1'
        original_run=json.loads(Path(prefix+'.run.json').read_text())
        native_run=json.loads(Path(str(a.capture).removesuffix('.png')+'.run.json').read_text())
        require(original_run['exit_code']==native_run['exit_code']==0,'Incomplete scene capture')
        for suffix,sha in {
            '.transition.bin':'60cc9ecd3c72d8ea87413ae2959ad51ce65b6aa3fca998cb451b06225204dbd5',
            '.camera.bin':'eb13865593317a4a589f075021bfcccf70dd6106536bbd3c10143827f34bd11e',
        }.items():require(digest(Path(prefix+suffix))==sha,'Changed next-night corpus')
        native=lambda suffix:Path(str(a.capture)+suffix)
        manager=rows(native('.interaction.bin').read_bytes(),8)
        camera=rows(native('.camera.bin').read_bytes(),16)
        transitions=rows(native('.transition.bin').read_bytes(),8)
        require([r[0] for r in transitions if r[3:5]==(1,0) and r[6]]==[16229],'Different night entry')
        original=rows(Path(prefix+'.transition.bin').read_bytes(),128)
        require([(r[0],r[3]) for r in original if r[1]==0x1bb7e0]==[(297,60)],'Different original night dispatch')
        managers=[r for r in original if r[1]==0x133a98 and r[0]>=298]
        require([r[0] for r in managers]==list(range(298,600)),'Night manager extent')
        for r in managers:
            n=manager[15930+r[0]]
            require((n[4],n[7],n[5],n[6])==(int(bool(r[35]&256)),r[36],r[48],r[118]),'Night manager differs')
        cameras=[r for r in rows(Path(prefix+'.camera.bin').read_bytes(),32) if r[0]>=297]
        require([r[0] for r in cameras]==list(range(297,600)),'Night camera extent')
        differences=[]
        for r in cameras:
            expected=r[4:8]+r[12:16]+r[8:12];actual=camera[15931+r[0]][4:16]
            for i,(want,got) in enumerate(zip(expected,actual)):
                if want!=got:
                    require(r[0]>=357 and i in [4,5,6],'Unexpected night camera difference')
                    differences.append(dict(original_frame=r[0],native_frame=r[0]+15932,component=i,original=want,native=got))
        require(len(differences)==714,'Changed known camera-target discrepancy')
        report['integration']=dict(original_run=original_run,native_run=native_run,
            original_dispatch=297,native_dispatch=16229,manager_samples=302,exact_manager_words=1208,
            first_camera_updates_exact=60,first_camera_words_exact=720,
            later_camera_target_differing_words=len(differences),first_differences=differences[:6],
            trace_sha256={str(f):digest(f) for f in [Path(prefix+s) for s in ['.transition.bin','.camera.bin']] +
                          [native(s) for s in ['.interaction.bin','.camera.bin','.transition.bin']]},
            limit='Manager execution matches; later camera targets and full raster fidelity remain unresolved')
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print('Event fade comparison exact:',len(records),'cases,',len(records)*6,'words')

if __name__=='__main__':main()
