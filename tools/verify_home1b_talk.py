#!/usr/bin/env python3
"""Audit successive HOME1B family conversation intervals.

Original save continuity fixes each native anchor. Requested pad times are not
used as observation times. State equality is not a full raster/audio claim.
"""
import argparse
import json
import math
import struct
from pathlib import Path
from verify_event import digest,write_exact
from verify_message_opening import rows,require
from verify_interaction_scene import actor_rows

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--mother',action='store_true');p.add_argument('--annie',action='store_true');p.add_argument('--warning',action='store_true')
    p.add_argument('--date',action='store_true');p.add_argument('--closure',action='store_true')
    p.add_argument('--door',action='store_true');a=p.parse_args()
    require(not a.annie or a.mother,'Annie comparison requires Mother interval')
    require(not a.warning or a.annie,'Warning comparison requires Annie interval')
    require(not a.date or a.warning,'Date comparison requires warning interval')
    require(not a.closure or a.date,'Closure comparison requires date interval')
    require(not a.door or a.closure,'Door comparison requires closure interval')
    native=lambda s:Path(str(a.capture)+s)
    run=json.loads(Path(str(a.capture).removesuffix('.png')+'.run.json').read_text())
    require(run['exit_code']==0,'Incomplete native capture')
    dialogue=rows(native('.dialogue.bin').read_bytes(),12)
    managers=rows(native('.interaction.bin').read_bytes(),8)
    cameras=rows(native('.camera.bin').read_bytes(),16)
    camera_states=rows(native('.camera-state.bin').read_bytes(),5)
    actors=actor_rows(native('.actor-1.bin'))
    require(len(dialogue)==len(managers)==len(cameras)==len(camera_states)==len(actors),'Native trace extent')
    require([r[0] for r in managers]==list(range(1,len(managers)+1)),'Native frame sequence')
    specs=[('local/reference-home1b-confirm-280-v1',16781,280,'local/reference-next-night-home1b-360-v1.state.zip')]
    if a.mother:specs.append(('local/reference-home1b-mother-420-v1',17061,420,'local/reference-home1b-confirm-280-v1.state.zip'))
    if a.annie:specs.append(('local/reference-home1b-annie-300-v1',17481,300,'local/reference-home1b-mother-420-v1.state.zip'))
    if a.warning:specs.append(('local/reference-home1b-warning-300-v1',17781,300,'local/reference-home1b-annie-300-v1.state.zip'))
    if a.date:specs.append(('local/reference-home1b-date-420-v1',18081,420,'local/reference-home1b-warning-300-v1.state.zip'))
    if a.closure:specs.append(('local/reference-home1b-festival-420-v1',18501,80,'local/reference-home1b-date-420-v1.state.zip'))
    animation_specs=specs[3:].copy()
    if a.door:specs[-1]=('local/reference-home1b-door-240-v2',18501,240,'local/reference-home1b-date-420-v1.state.zip')
    artifacts=[native(s) for s in ['.dialogue.bin','.interaction.bin','.camera.bin','.camera-state.bin','.actor-1.bin']]
    results=[]
    for prefix,first,total,source in specs:
        ref=lambda s:Path(prefix+s)
        original_run=json.loads(ref('.run.json').read_text())
        require(original_run['exit_code']==0 and source in original_run['command'],'Original completion/save continuity')
        captured=int(original_run['command'][2]);require(total<=captured,'Original capture shorter than comparison')
        log=ref('.log').read_text()
        require(f'Loaded reference state {source}' in log and f'Stopped: vblanks={captured} exit_request=0' in log,'Original log completion')
        require(first+total-1<=len(managers),'Native interval incomplete')
        controller=rows(ref('.controller.bin').read_bytes(),64);require(len(controller)%2==0,'Unpaired controller trace')
        pressed=[];seen=[]
        for before,after in zip(controller[::2],controller[1::2]):
            frame=after[0]
            require(before[:2]==(frame,0x148dc8) and after[1]==0x11ff38,'Controller pairing')
            if frame>=total:continue
            require(0<=frame<total and (not seen or frame>seen[-1]),'Controller order');seen.append(frame)
            n=dialogue[first+frame-1]
            expected=(after[32],after[33],after[34],after[35],after[36],after[16],after[21],after[17],after[7],after[43])
            require(n[2:]==expected,f'Controller differs: {prefix} frame{frame}')
            require(managers[first+frame-1][1]==after[9],f'Pressed input differs at {prefix} frame{frame}')
            if after[9]:pressed.append([frame,first+frame,after[9]])
        original=[r for r in rows(ref('.transition.bin').read_bytes(),128) if r[1]==0x133a98]
        require([r[0] for r in original]==list(range(captured)),'Original manager extent')
        original=original[:total]
        for r in original:
            n=managers[first+r[0]-2]
            require(managers[first+r[0]-1][1]==r[33]>>16,f'Manager pad latch differs: {prefix} frame{r[0]}')
            require((n[4],n[7],n[5],n[6])==(int(bool(r[35]&256)),r[36],r[48],r[118]),f'Manager differs: {prefix} frame{r[0]}')
            require(actors[first+r[0]-2][8:16]==r[40:48],f'HERO differs: {prefix} frame{r[0]}')
        camera=rows(ref('.camera.bin').read_bytes(),32)
        require([r[0] for r in camera]==list(range(captured)),'Original camera extent')
        closure=first==18501
        camera_error=0.
        for r in camera[:total]:
            at=first+r[0]-1;n=cameras[at]
            if closure:
                require(n[12:16]==r[8:12],'Closure camera rotation differs')
                require((camera_states[at][1],camera_states[at][3])==r[2:4],'Closure camera mode/phase differs')
                actual=struct.unpack('<4f',struct.pack('<4I',*n[4:8]))
                expected=struct.unpack('<4f',struct.pack('<4I',*r[4:8]))
                require(all(math.isfinite(v) for v in actual+expected),'Nonfinite camera position')
                error=max(abs(x-y) for x,y in zip(actual,expected));camera_error=max(camera_error,error)
                require(error<=0.00002,'Closure camera position exceeds anchored-helper tolerance')
            else:require(n[4:16]==r[4:8]+r[12:16]+r[8:12],f'Camera differs: {prefix} frame{r[0]}')
        camera_result=(dict(exact_camera_rotation_words=total*4,exact_camera_mode_phase_words=total*2,
                            camera_position_maximum_error=camera_error,camera_position_tolerance=0.00002,
                            camera_target_limit='Original requested global and native effective blended target are different observables; not compared during closure.')
                       if closure else dict(exact_camera_words=total*12))
        results.append(dict(prefix=prefix,native_first_frame=first,original_run=original_run,compared_frames=total,
                            controller_returns=len(seen),exact_controller_words=len(seen)*10,
                            manager_samples=total,exact_manager_words=total*4,
                            hero_samples=total,exact_hero_words=total*8,
                            camera_updates=total,**camera_result,observed_presses=pressed,
                            pad_latch_samples=total,
                            manager_observed_presses=[[r[0],first+r[0],r[33]>>16] for r in original if r[33]>>16],
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
        animation=[]
        for prefix,first,total,_ in animation_specs:
            path=Path(prefix+'.actor.bin')
            reference=rows(path.read_bytes(),8+174*4)
            require([r[0] for r in reference[:total]]==list(range(total)),'Annie actor interval')
            for r in reference[:total]:
                require(r[4:8]==(6,174,0,0x133a98),'Annie instance/channel/boundary changed')
                n=native_actor[first-1+r[0]]
                require(n[1:4]==r[1:4] and n[5]==174,'Annie field/motion state differs')
                require(n[8:8+174*4]==r[8:],'Annie channels differ at '+prefix+' frame'+str(r[0]))
            animation.append(dict(prefix=prefix,actor_alias=-17,original_instance=6,channels=174,manager_observations=total,
                                  exact_channel_words=total*174*4,exact_field_motion_words=total*3))
            artifacts.append(path)
        artifacts.append(native('.actor-17.bin'))
    write_exact(a.output,(json.dumps(dict(scope=__doc__,native_run=run,intervals=results,annie_animation=animation,
                 sha256={str(f):digest(f) for f in artifacts},
                 limits=['Rendering, visibility flags, other actors and audio are outside these comparisons.',
                         'Original controller is not observed during the explicit closure/initialization gaps.']),indent=2)+'\n').encode())
    print('HOME1B conversation intervals validated:',len(results))

if __name__=='__main__':main()
