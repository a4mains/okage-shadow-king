#!/usr/bin/env python3
"""Audit normal field message99099 opening, confirmation, closure and handoff.

Covers90 constructor words,180 controller returns,300 field-manager samples and
observed pressed latches. UI geometry, full rendering/audio, movement trajectories
and cold game construction remain outside this comparison.
"""
import argparse
import json
from pathlib import Path
import subprocess
from verify_message_opening import initialization_projection, compare_controller, rows, require, digest
from verify_event import write_exact

INTERVALS = [
    ('reference-field-exit-180-v2',7961,180,41,139,
     'bfa96a167d7bdb3a84eabf63dfffaf0049f316c43d6905f0a2be77e4735d6308',
     'c59575f0bf437533cd740fb8ca2e984be487077ba4ef9a1cc96f230b668f2eb2'),
    ('reference-gate-message-close-120-v1',8141,120,0,41,
     '74e7aaff83a14d2ad0d15d61baf339f95230a53a8170f5fe2c05dfd53ca568eb',
     '260f879ef792c806c6e1c54cf8d9d5a66919bafafd147b26bcb08edc3ac054f5')]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture',type=Path,required=True)
    parser.add_argument('--native',type=Path,default=Path('build/native/okage-assets'))
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();base=str(args.capture)
    path=lambda suffix:Path(base+suffix)
    manifest=Path(base.removesuffix('.png')+'.run.json')
    run=json.loads(manifest.read_text());require(run['exit_code']==0,'Incomplete native capture')
    dialogue=path('.dialogue.bin').read_bytes();states=rows(dialogue,12)
    choices=rows(path('.choices.bin').read_bytes(),12)
    interactions=rows(path('.interaction.bin').read_bytes(),8)
    total=len(states);require(total>=8260 and len(choices)==len(interactions)==total,'Native capture extent')
    for source in [choices,interactions]:require([r[0] for r in source]==list(range(1,total+1)),'Native frame identity')
    original=Path('local/message-init-system-v1/99099.original.bin')
    require(digest(original)=='2b82605c02817c01bdaa3cbb9d191ce24fc30fef607ef14e8d5da243e9170379','Changed original constructor')
    init_manifest=Path('local/message-init-system-v1/original.run.json')
    init_run=json.loads(init_manifest.read_text());require(init_run['exit_code']==0,'Incomplete constructor probe')
    output=args.output.with_suffix('.init.bin');require(not output.exists(),'Initialization output exists')
    command=[str(args.native),'field-message-init','local/text-input/FLDDATA/MES/FIELD.PME','99099',str(output)]
    subprocess.run(command,check=True)
    require(output.read_bytes()==initialization_projection(original.read_bytes(),99099),'Initialization differs')
    files=[args.capture,manifest,args.native,original,init_manifest,output,path('.dialogue.bin'),path('.choices.bin'),path('.interaction.bin')]
    reports=[]
    for name,anchor,count,first,frames,controller_sha,manager_sha in INTERVALS:
        prefix='local/'+name;controller=Path(prefix+'.controller.bin');manager=Path(prefix+'.interaction.bin')
        require(digest(controller)==controller_sha and digest(manager)==manager_sha,'Changed original system-message trace')
        original_manifest=Path(prefix+'.run.json');original_run=json.loads(original_manifest.read_text())
        require(original_run['exit_code']==0,'Incomplete original system-message run')
        compare_controller(controller.read_bytes(),dialogue,first_original=first,first_native=anchor+first,
                           frames=frames,total=total,message_id=99099)
        for r in rows(controller.read_bytes(),64)[::2]:
            require(choices[anchor+r[0]-1][9]==r[9],'Controller pressed latch differs')
        source=rows(manager.read_bytes(),176);entries=[r for r in source if r[1]==0x133a98]
        require([r[0] for r in entries]==list(range(count)),'Original manager extent')
        for r in entries:
            n=interactions[anchor+r[0]-2]
            require((n[4],n[7],n[5],n[6])==(int(bool(r[20]&0x100)),r[21],r[15],r[150]),
                    f'Manager differs at {name}:{r[0]}')
        if count==180:
            require([(r[0],r[3]) for r in source if r[1]==0x1bb7e0]==[(3,4),(41,3)],'Event dispatch differs')
        else:
            require([(r[0],r[9],r[15]) for r in source if r[1]==0x132ca8]==[(44,5,1)],'Handoff differs')
        reports.append(dict(reference=name,original_run=original_run,anchor=anchor,controller_words=frames*10,manager_words=count*4))
        files.extend([controller,manager,original_manifest])
    require(states[8179][10]==9 and interactions[8183][4:7]==(0,1,1) and interactions[8184][4:7]==(0,0,1),'Native closure/handoff differs')
    before='local/native-field-gate-7960-camera-v2.png'
    for suffix in ['.dialogue.bin','.choices.bin','.interaction.bin','.movement.bin','.camera.bin','.actor-1.bin']:
        old=Path(before+suffix);require(path(suffix).read_bytes().startswith(old.read_bytes()),'Prior route changed: '+suffix)
        files.extend([old,path(suffix)])
    report=dict(scope=__doc__,native_run=run,init_run=init_run,init_command=command,intervals=reports,
                initialization_words=90,controller_words=1800,manager_words=1200,pressed_latches=180,
                closure_frame=8180,script_finished_frame=8184,control_return_frame=8185,
                unchanged_prefix_frames=7960,sha256={str(p):digest(p) for p in files})
    write_exact(args.output,(json.dumps(report,indent=2)+'\n').encode())
    print('90 initialization words,180 controller returns,300 manager samples and system-message closure match')

if __name__=='__main__':main()
