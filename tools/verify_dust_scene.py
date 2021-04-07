#!/usr/bin/env python3
"""Audit native outdoor movement/dust lifecycle and observed original scheduling.

Does not compare whole-scene pixels, NPC/global RNG, camera trajectories or
original/native root coordinates. Original field timing is independently observed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from verify_event import write_exact
from verify_choice_scene import rows
from verify_interaction_scene import actor_rows

def require(value, why):
    if not value:raise ValueError(why)

def map_rows(data):
    at=0;result=[]
    while at<len(data):
        require(len(data)-at>=8,'Truncated effect frame')
        frame,count=struct.unpack_from('<2I',data,at);at+=8
        require(frame==len(result)+1 and count<=84,'Effect frame identity/count')
        jobs={}
        for _ in range(count):
            require(len(data)-at>=88,'Truncated effect job')
            header=struct.unpack_from('<22I',data,at);at+=88
            handle,template,flags,phase,duration,n=header[:6]
            require(handle<84 and handle not in jobs and n<=128,'Effect job identity/count')
            require(len(data)-at>=n*80,'Truncated effect elements')
            elements=list(struct.iter_unpack('<20I',data[at:at+n*80]));at+=n*80
            jobs[handle]=(header,elements)
        result.append(jobs)
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,default=Path('local/native-outdoor-controls-5650-dust-order-v1.png'))
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    prefix=str(args.capture)
    run=json.loads(Path(prefix.removesuffix('.png')+'.run.json').read_text())
    require(run['exit_code']==0,'Incomplete native route')
    state=json.loads(Path(prefix+'.json').read_text())
    require(state['frames']==5650 and not state['field_event_running'],'Incomplete native controls')
    jobs=map_rows(Path(prefix+'.map-effects.bin').read_bytes())
    movement=rows(Path(prefix+'.movement.bin'),16)
    hero=actor_rows(Path(prefix+'.actor-1.bin'))
    camera=rows(Path(prefix+'.camera.bin'),16)
    require(all(len(r)==5650 for r in (jobs,movement,hero,camera)),'Incomplete native trace interval')
    require(all(r[1]&0xf000==0x4000 for r in movement[5599:5619]),'Native movement input differs')
    require(all(r[1]&0xf000==0 for r in movement[5619:]),'Native input did not release')
    require(all(r[2]==0 for r in movement[5599:]),'Native movement remains gated')
    require(hero[5598][12:16]!=hero[5619][12:16],'Native HERO did not move')
    require(all(r[12:16]==hero[-1][12:16] for r in hero[5620:]),'Native HERO did not stop')
    require(camera[5598][4:7]!=camera[5619][4:7],'Native camera did not respond')
    spawns=[]
    for i,frame_jobs in enumerate(jobs):
        for handle,(header,_) in frame_jobs.items():
            if header[1]!=17:continue
            if i and handle in jobs[i-1] and jobs[i-1][handle][0][1]==17:continue
            require(header[6:10]==hero[i][12:16],'Dust does not use spawn-time HERO position')
            spawns.append((i+1,handle,header[6:10]))
    require([(f,h) for f,h,_ in spawns]==[(5601,1),(5609,3),(5617,0)],'Dust allocation/cadence differs')
    for start,handle,position in spawns:
        for age in range(33):
            header,_=jobs[start-1+age][handle]
            phase=min(age+1,31)
            flags=0 if age==0 else 0x120 if age==31 else 0x320 if age==32 else 0x20
            require(header[1:6]==(17,flags,phase,30,9),'Dust update/removal scheduling differs')
            require(header[6:10]==position,'Dust follows HERO after spawning')
        require(handle not in jobs[start+32],'Dust slot was not released')
    require(not any(h[0][1]==17 for h in jobs[-1].values()),'Dust remained after release')
    original=Path('local/reference-exterior-dust-65-v1')
    order=Path('local/reference-exterior-dust-order-12-v2')
    for source,n in [(original,65),(order,12)]:
        require(json.loads(Path(str(source)+'.run.json').read_text())['exit_code']==0,'Incomplete original run')
        require(f'Stopped: vblanks={n} exit_request=0' in Path(str(source)+'.log').read_text(),'Original interval incomplete')
    observed=rows(Path(str(original)+'.dust.bin'),16)
    original_spawns=[r for r in observed if r[1]==0x1012f8]
    require([r[0] for r in original_spawns]==[2,10,18],'Original dust cadence differs')
    for spawn,handle in zip(original_spawns,[1,3,0]):
        updates=[r for r in observed if r[1]==0x1131d0 and r[2]==handle]
        require([r[0] for r in updates]==list(range(spawn[0]+1,spawn[0]+32)),'Original dust update timing differs')
        require([r[13] for r in updates]==list(range(1,32)),'Original dust phases differ')
        require(all(r[8:12]==spawn[8:12] for r in updates),'Original dust attachment changed')
        removed=[r for r in observed if r[1]==0x103640 and r[2]==handle]
        require(len(removed)==1 and removed[0][0]==spawn[0]+31,'Original dust removal timing differs')
    ordering=rows(Path(str(order)+'.dust.bin'),16)
    for frame in range(12):
        sequence=[r[1] for r in ordering if r[0]==frame and r[1] in (0x133a98,0x102218,0x138130,0x134c18)]
        require(sequence==[0x133a98,0x102218]+[0x138130]*7+[0x134c18],'Original manager/effects/NPC/player order differs')
    files=[args.capture,Path(prefix+'.map-effects.bin'),Path(prefix+'.movement.bin'),Path(prefix+'.actor-1.bin'),
           Path(prefix+'.camera.bin'),Path(str(original)+'.dust.bin'),Path(str(order)+'.dust.bin')]
    report=dict(scope=__doc__,native_run=run,spawn_frames=[f for f,_,_ in spawns],handles=[h for _,h,_ in spawns],
                original_spawn_frames=[r[0] for r in original_spawns],first_update_delay=1,
                native_removal_request_age=31,native_slot_release_age=33,original_order_frames=12,
                native_movement_and_stop=True,native_camera_response=True,
                sha256={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files})
    write_exact(args.output,(json.dumps(report,indent=2)+'\n').encode())
    print('Native movement/stop, three dust lifetimes and original scheduling verified')

if __name__=='__main__':main()
