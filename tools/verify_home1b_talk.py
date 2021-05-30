#!/usr/bin/env python3
"""Audit HOME1B Father closure and optionally Mother's conversation.

Original save continuity fixes each native anchor. Requested pad times are not
used as observation times. State equality is not a full raster/audio claim.
"""
import argparse
import json
import struct
from pathlib import Path
from verify_event import digest,write_exact
from verify_message_opening import rows,require
from verify_interaction_scene import actor_rows

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--mother',action='store_true');p.add_argument('--annie',action='store_true');p.add_argument('--warning',action='store_true');a=p.parse_args()
    require(not a.annie or a.mother,'Annie comparison requires Mother interval')
    require(not a.warning or a.annie,'Warning comparison requires Annie interval')
    native=lambda s:Path(str(a.capture)+s)
    run=json.loads(Path(str(a.capture).removesuffix('.png')+'.run.json').read_text())
    require(run['exit_code']==0,'Incomplete native capture')
    dialogue=rows(native('.dialogue.bin').read_bytes(),12)
    managers=rows(native('.interaction.bin').read_bytes(),8)
    cameras=rows(native('.camera.bin').read_bytes(),16)
    actors=actor_rows(native('.actor-1.bin'))
    require(len(dialogue)==len(managers)==len(cameras)==len(actors),'Native trace extent')
    require([r[0] for r in managers]==list(range(1,len(managers)+1)),'Native frame sequence')
    specs=[('local/reference-home1b-confirm-280-v1',16781,280,'local/reference-next-night-home1b-360-v1.state.zip')]
    if a.mother:specs.append(('local/reference-home1b-mother-420-v1',17061,420,'local/reference-home1b-confirm-280-v1.state.zip'))
    if a.annie:specs.append(('local/reference-home1b-annie-300-v1',17481,300,'local/reference-home1b-mother-420-v1.state.zip'))
    if a.warning:specs.append(('local/reference-home1b-warning-300-v1',17781,300,'local/reference-home1b-annie-300-v1.state.zip'))
    artifacts=[native(s) for s in ['.dialogue.bin','.interaction.bin','.camera.bin','.actor-1.bin']]
    results=[]
    for prefix,first,total,source in specs:
        ref=lambda s:Path(prefix+s)
        original_run=json.loads(ref('.run.json').read_text())
        require(original_run['exit_code']==0 and source in original_run['command'],'Original completion/save continuity')
        log=ref('.log').read_text()
        require(f'Loaded reference state {source}' in log and f'Stopped: vblanks={total} exit_request=0' in log,'Original log completion')
        require(first+total-1<=len(managers),'Native interval incomplete')
        controller=rows(ref('.controller.bin').read_bytes(),64);require(len(controller)%2==0,'Unpaired controller trace')
        pressed=[];seen=[]
        for before,after in zip(controller[::2],controller[1::2]):
            frame=after[0]
            require(before[:2]==(frame,0x148dc8) and after[1]==0x11ff38,'Controller pairing')
            require(0<=frame<total and (not seen or frame>seen[-1]),'Controller order');seen.append(frame)
            n=dialogue[first+frame-1]
            expected=(after[32],after[33],after[34],after[35],after[36],after[16],after[21],after[17],after[7],after[43])
            require(n[2:]==expected,f'Controller differs: {prefix} frame{frame}')
            require(managers[first+frame-1][1]==after[9],f'Pressed input differs at {prefix} frame{frame}')
            if after[9]:pressed.append([frame,first+frame,after[9]])
        original=[r for r in rows(ref('.transition.bin').read_bytes(),128) if r[1]==0x133a98]
        require([r[0] for r in original]==list(range(total)),'Original manager extent')
        for r in original:
            n=managers[first+r[0]-2]
            require((n[4],n[7],n[5],n[6])==(int(bool(r[35]&256)),r[36],r[48],r[118]),f'Manager differs: {prefix} frame{r[0]}')
            require(actors[first+r[0]-2][8:16]==r[40:48],f'HERO differs: {prefix} frame{r[0]}')
        camera=rows(ref('.camera.bin').read_bytes(),32)
        require([r[0] for r in camera]==list(range(total)),'Original camera extent')
        for r in camera:require(cameras[first+r[0]-1][4:16]==r[4:8]+r[12:16]+r[8:12],f'Camera differs: {prefix} frame{r[0]}')
        results.append(dict(prefix=prefix,native_first_frame=first,original_run=original_run,
                            controller_returns=len(seen),exact_controller_words=len(seen)*10,
                            manager_samples=total,exact_manager_words=total*4,
                            hero_samples=total,exact_hero_words=total*8,
                            camera_updates=total,exact_camera_words=total*12,observed_presses=pressed,
                            controller_unobserved_frames=[f for f in range(total) if f not in seen]))
        artifacts += [ref(s) for s in ['.run.json','.transition.bin','.controller.bin','.camera.bin','.state.zip','.input']]
    animation=None
    if a.warning:
        data=native('.actor-17.bin').read_bytes();at=0;native_actor={}
        while at<len(data):
            require(len(data)-at>=32,'Native actor header extent')
            header=struct.unpack_from('<8I',data,at);size=8+header[5]*4+header[6]
            require(4<=header[5]<=5120 and header[6]<=65536 and size*4<=len(data)-at,'Native actor record extent')
            row=struct.unpack_from('<'+'I'*size,data,at);at+=size*4
            require(not native_actor or row[0]>next(reversed(native_actor)),'Native actor frame order')
            native_actor[row[0]]=row
        path=Path('local/reference-home1b-warning-300-v1.actor.bin')
        reference=rows(path.read_bytes(),8+174*4)
        require([r[0] for r in reference]==list(range(300)),'Annie actor interval')
        for r in reference:
            require(r[4:8]==(6,174,0,0x133a98),'Annie instance/channel/boundary changed')
            n=native_actor[17780+r[0]]
            require(n[1:4]==r[1:4] and n[5]==174 and n[7]==1,'Annie field/motion state differs')
            require(n[8:8+174*4]==r[8:],'Annie channels differ at original'+str(r[0]))
        animation=dict(actor_alias=-17,original_instance=6,channels=174,manager_observations=300,
                       exact_channel_words=208800,exact_field_motion_words=900)
        artifacts += [path,native('.actor-17.bin')]
    write_exact(a.output,(json.dumps(dict(scope=__doc__,native_run=run,intervals=results,annie_animation=animation,
                 sha256={str(f):digest(f) for f in artifacts},
                 limits=['Rendering, other actors and audio are outside these comparisons.',
                         'Original controller is not observed during the explicit closure/initialization gaps.']),indent=2)+'\n').encode())
    print('HOME1B conversation intervals exact:',len(results))

if __name__=='__main__':main()
