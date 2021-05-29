#!/usr/bin/env python3
"""Audit native HOME1B startup and Father's first dialogue against original traces.

Observer alignment derives from startup50 dispatch. This does not compare full
raster output, the next dialogue confirmation, physical input or audio fidelity.
"""
import argparse
import json
from pathlib import Path
from verify_event import digest,write_exact
from verify_message_opening import require,rows,compare_controller
from verify_interaction_scene import actor_rows

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();prefix='local/reference-next-night-home1b-360-v1'
    original_run=json.loads(Path(prefix+'.run.json').read_text())
    native_run=json.loads(Path(str(a.capture).removesuffix('.png')+'.run.json').read_text())
    require(original_run['exit_code']==native_run['exit_code']==0,'Incomplete capture')
    native=lambda s:Path(str(a.capture)+s)
    state=json.loads(native('.json').read_text())
    require(state['message_id']==11301 and state['controller_stage']==5,'Family dialogue not reached')
    require(state['scene_brightness']==1,'Room still faded out')
    m=rows(native('.interaction.bin').read_bytes(),8);c=rows(native('.camera.bin').read_bytes(),16)
    tr=rows(native('.transition.bin').read_bytes(),8);actors=actor_rows(native('.actor-1.bin'))
    require(len(m)==len(c)==len(tr)==len(actors)==17000,'Native extent')
    require([r[0] for r in m]==list(range(1,17001)),'Native frame ordering')
    require([r[0] for r in tr if r[3:5]==(71,0) and r[6]]==[16534],'Changed native entry')
    o=rows(Path(prefix+'.transition.bin').read_bytes(),128)
    require([(r[0],r[2],r[3]) for r in o if r[1]==0x1bb7e0]==[(113,0x1326f8,50)],'Changed original dispatch')
    managers=[r for r in o if r[1]==0x133a98 and r[0]>=114]
    require([r[0] for r in managers]==list(range(114,360)),'Original manager extent')
    for r in managers:
        n=m[r[0]+16421-2]
        require((n[4],n[7],n[5],n[6])==(int(bool(r[35]&256)),r[36],r[48],r[118]),'HOME1B manager differs')
        require(actors[r[0]+16421-2][8:16]==r[40:48],'HOME1B HERO differs')
    cameras=[r for r in rows(Path(prefix+'.camera.bin').read_bytes(),32) if r[0]>=113]
    require([r[0] for r in cameras]==list(range(113,360)),'Original camera extent')
    for r in cameras:require(c[r[0]+16421-1][4:16]==r[4:8]+r[12:16]+r[8:12],'HOME1B camera differs')
    count=compare_controller(Path(prefix+'.controller.bin').read_bytes(),native('.dialogue.bin').read_bytes(),
                             first_original=143,first_native=16564,frames=217,total=len(m),message_id=11301)
    require(m[-1][7]==0xf10 and m[-1][4]==1,'Family event state changed')
    baseline='local/native-next-night-17000-release-v2.png'
    prefixes={'.interaction.bin':8,'.dialogue.bin':12,'.choices.bin':12,'.camera.bin':16,
              '.camera-state.bin':5,'.transition.bin':8,'.music.bin':5}
    compared={}
    for suffix,width in prefixes.items():
        size=16533*width*4
        old=Path(baseline+suffix).read_bytes()[:size];new=native(suffix).read_bytes()[:size]
        require(len(old)==len(new)==size and old==new,'Earlier native prefix changed: '+suffix)
        compared[suffix]=size
    size=16533*3200
    old_pcm=Path(baseline+'.audio.pcm').read_bytes()[:size];new_pcm=native('.audio.pcm').read_bytes()[:size]
    require(len(old_pcm)==len(new_pcm)==size and old_pcm==new_pcm,'Prior PCM changed or truncated')
    files=[Path(prefix+s) for s in ['.transition.bin','.camera.bin','.controller.bin','.state.zip','.run.json']]
    files += [native(s) for s in ['.interaction.bin','.camera.bin','.dialogue.bin','.actor-1.bin','.transition.bin','.json']]
    report=dict(scope=__doc__,original_run=original_run,native_run=native_run,original_dispatch=113,native_dispatch=16534,
                manager_samples=246,exact_manager_words=984,hero_samples=246,exact_hero_words=1968,
                camera_updates=247,exact_camera_words=2964,controller_returns=count,exact_controller_words=count*10,
                native_message_open_frame=16564,final_event_pc='0xf10',scene_brightness=1,
                unchanged_native_prefix_frames=16533,unchanged_trace_bytes=compared,unchanged_pcm_bytes=size,
                sha256={str(f):digest(f) for f in files},
                limits=['Diagnostic dialogue panel, shadows, lighting and full raster fidelity remain open.',
                        'Earlier bakery/night camera discrepancies and narration asynchronous-loading gap remain.',
                        'No input after15622; family conversation confirmation/closure is not yet demonstrated.'])
    write_exact(a.output,(json.dumps(report,indent=2)+'\n').encode())
    print('HOME1B startup, HERO, camera and opening dialogue match original; native prefix unchanged')

if __name__=='__main__':main()
