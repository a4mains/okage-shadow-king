#!/usr/bin/env python3
"""Audit alternate village entry and optionally Annie's return conversation.

Dispatch-aligned manager/HERO/camera state comparisons, not whole-scene raster,
audio or completed conversation claims. Reads completed captures only.
"""
import argparse
import json
import struct
from pathlib import Path
from prepare_assets import ELF_HASH
from verify_event import digest, write_exact
from verify_message_opening import require, rows, compare_controller
from verify_interaction_scene import actor_rows

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--dialogue',action='store_true')
    p.add_argument('--answer',action='store_true')
    p.add_argument('--narration',action='store_true')
    a=p.parse_args()
    require(not a.answer or a.dialogue,'Answer comparison requires --dialogue')
    require(not a.narration or a.answer,'Narration comparison requires --answer')
    require(digest(Path('local/extracted/SCUS_971.29'))==ELF_HASH,'Different ELF')
    prefix='local/reference-bakery-exit-300-v1'
    run=json.loads(Path(prefix+'.run.json').read_text())
    native_run=json.loads(Path(str(a.capture).removesuffix('.png')+'.run.json').read_text())
    require(run['exit_code']==native_run['exit_code']==0,'Incomplete capture')
    native=lambda suffix:Path(str(a.capture)+suffix)
    manager=rows(native('.interaction.bin').read_bytes(),8)
    camera=rows(native('.camera.bin').read_bytes(),16)
    state=rows(native('.camera-state.bin').read_bytes(),5)
    actors=actor_rows(native('.actor-1.bin'))
    transitions=rows(native('.transition.bin').read_bytes(),8)
    require(len(manager)>=14920 and [r[0] for r in manager]==list(range(1,len(manager)+1)),'Native extent')
    require(len(camera)==len(state)==len(actors)==len(transitions)==len(manager),'Native trace extent')
    entry=[r[0] for r in transitions if r[3:5]==(0,4) and r[6]==1]
    require(entry==[14674],'Different native village return')
    for suffix,sha in {'.transition.bin':'f9f7cbe13ff66c75a87110d7d569e2366c376ab001323b1e191e355473cfc8db',
                       '.camera.bin':'01411517e0c6f8db52a9ec7ad29360ff814bcdd0bfcb2cd1be326aa61bc192a6'}.items():
        require(digest(Path(prefix+suffix))==sha,'Changed original village-return corpus')
    original=rows(Path(prefix+'.transition.bin').read_bytes(),128)
    require([(r[0],r[2],r[3]) for r in original if r[1]==0x1bb7e0]==
            [(51,0x1326f8,1),(222,0x1326f8,60)],'Different original dispatches')
    delta=entry[0]-222
    managers=[r for r in original if r[1]==0x133a98 and r[0]>=223]
    require([r[0] for r in managers]==list(range(223,300)),'Original manager interval')
    for r in managers:
        n=manager[r[0]+delta-2]
        require((n[4],n[7],n[5],n[6])==(int(bool(r[35]&256)),r[36],r[48],r[118]),'Return manager differs')
        require(actors[r[0]+delta-2][8:16]==r[40:48],'Return HERO roots differ')
    cameras=[r for r in rows(Path(prefix+'.camera.bin').read_bytes(),32) if r[0]>=222]
    require([r[0] for r in cameras]==list(range(222,300)),'Original camera interval')
    for r in cameras:
        i=r[0]+delta-1
        require(camera[i][4:16]==r[4:8]+r[12:16]+r[8:12],'Return camera differs')
        require((state[i][1],state[i][3])==(r[2],r[3]),'Return camera mode/phase differs')
    files=[Path(prefix+s) for s in ['.transition.bin','.camera.bin']]
    files += [native(s) for s in ['.interaction.bin','.camera.bin','.camera-state.bin','.actor-1.bin','.transition.bin']]
    report=dict(scope=__doc__,elf_sha256=ELF_HASH,native_run=native_run,original_run=run,
                native_dispatch=14674,original_dispatch=222,manager_samples=77,exact_manager_words=308,
                hero_samples=77,exact_hero_words=616,camera_updates=78,exact_camera_words=936,
                exact_camera_mode_phase_words=156,sha256={str(f):digest(f) for f in files})
    if a.dialogue:
        talk='local/reference-village-return-talk-480-v1'
        talk_run=json.loads(Path(talk+'.run.json').read_text())
        require(talk_run['exit_code']==0,'Incomplete original conversation capture')
        for suffix,sha in {
            '.controller.bin':'aaf6724a6b6861eb18ea36dd956a0e2e4403998988449d9955a0d287ba3c513d',
            '.controller.bin.choices.bin':'84aafc3a51968606d6bf6435b543a2f3fc978f96c35fbc1af2a020fc84588eab',
            '.transition.bin':'b30ec8e966b414172668f778c095af7ccbe8405bd00c3390e79661cafae18763',
        }.items():
            require(digest(Path(talk+suffix))==sha,'Changed original return dialogue corpus')
        require(len(manager)>=15231,'Native dialogue comparison requires15231 frames')
        data=Path(talk+'.controller.bin').read_bytes()
        controllers=rows(data,64)
        require(controllers[0][0]==126 and controllers[-1][0]==479,'Different message opening/extent')
        count=compare_controller(data,native('.dialogue.bin').read_bytes(),first_original=126,
                                 first_native=14878,frames=354,total=len(manager),message_id=10201)
        pressed=[]
        for r in controllers[1::2]:
            require(manager[14751+r[0]][1]==r[9],'Return dialogue pressed timing differs')
            if r[9]:pressed.append(r[0])
        managers=[r for r in rows(Path(talk+'.transition.bin').read_bytes(),128) if r[1]==0x133a98]
        require([r[0] for r in managers]==list(range(480)),'Dialogue manager interval')
        for r in managers:
            n=manager[14750+r[0]]
            require((n[4],n[7],n[5],n[6])==(int(bool(r[35]&256)),r[36],r[48],r[118]),'Dialogue manager differs')
        choices=rows(Path(talk+'.controller.bin.choices.bin').read_bytes(),16)[1::2]
        native_choices=rows(native('.choices.bin').read_bytes(),12)
        require([r[0] for r in choices]==list(range(126,480)),'Dialogue choice interval')
        for r in choices:
            require(r[2:6]==native_choices[14751+r[0]][2:6],'Dialogue choice differs')
        require(native_choices[15230][6:8]==(1,0),'First choice is not waiting unarmed')
        report['dialogue']=dict(original_run=talk_run,controller_returns=354,exact_controller_words=count,
                               manager_samples=480,exact_manager_words=1920,choice_states=354,
                               exact_choice_words=1416,observed_pressed=pressed)
        for f in [Path(talk+s) for s in ['.controller.bin','.controller.bin.choices.bin','.transition.bin']] + [native(s) for s in ['.dialogue.bin','.choices.bin']]:
            report['sha256'][str(f)]=digest(f)
    if a.answer:
        answer='local/reference-village-return-answer-480-v1'
        answer_run=json.loads(Path(answer+'.run.json').read_text())
        require(answer_run['exit_code']==0 and len(manager)>=15711,'Incomplete answer comparison')
        for suffix,sha in {
            '.controller.bin':'8e141d494de31deeb1204ab50b5a866043e7e53f034a128ec32a86fa28a305c1',
            '.controller.bin.choices.bin':'249597f79a8e7e9da94f126b0d6f2b4852d768759fe019174789143451146f0e',
            '.transition.bin':'35bce2a3b7f5efc154d512ff1043fff7c34f57aaf9c9f7c5b190b3fae4e13fa7',
        }.items():
            f=Path(answer+suffix);require(digest(f)==sha,'Changed original answer corpus')
            report['sha256'][str(f)]=sha
        controllers=rows(Path(answer+'.controller.bin').read_bytes(),64)
        first=[r for r in controllers if r[0]<430]
        data=b''.join(struct.pack('<64I',*r) for r in first)
        count=compare_controller(data,native('.dialogue.bin').read_bytes(),first_original=0,
                                 first_native=15232,frames=430,total=len(manager),message_id=10201)
        pressed=[]
        for r in first[1::2]:
            require(manager[15231+r[0]][1]==r[9],'Answer pressed timing differs')
            if r[9]:pressed.append(r[0])
        require(pressed==[10,29,70,111,153,193,234,350,390],'Answer input corpus differs')
        managers=[r for r in rows(Path(answer+'.transition.bin').read_bytes(),128) if r[1]==0x133a98]
        require([r[0] for r in managers]==list(range(480)),'Answer manager interval')
        for r in managers:
            if 466<=r[0]<=471:continue
            n=manager[15230+r[0]]
            require((n[4],n[7],n[5],n[6])==(int(bool(r[35]&256)),r[36],r[48],r[118]),'Answer manager differs')
        gap=[]
        for r in managers[466:472]:
            n=manager[15230+r[0]]
            require((n[4],n[7],n[5],n[6])==(1,0x2f6c,1,1) and
                    (int(bool(r[35]&256)),r[36],r[118])==(1,0x2f50,1) and
                    r[17]==15 and r[28]==0,'Changed sound-load gap')
            gap.append(dict(original_frame=r[0],native_frame=n[0],original_pc=r[36],native_pc=n[7],
                            original_manager_mode=15,original_movement_observed=False))
        choices=rows(Path(answer+'.controller.bin.choices.bin').read_bytes(),16)[1::2]
        for r in choices:
            if r[0]<430:require(r[2:6]==native_choices[15231+r[0]][2:6],'Answer choice differs')
        accepts=[(r[0],r[5]) for i,r in enumerate(native_choices[15231:15661],15231)
                 if native_choices[i-1][7] and not r[7]]
        require(accepts==[(15261,0)],'Different accepted answer')
        narration=[r for r in controllers if r[0]>=471]
        data=b''.join(struct.pack('<64I',*r) for r in narration)
        words=compare_controller(data,native('.dialogue.bin').read_bytes(),first_original=471,
                                 first_native=15697,frames=9,total=len(manager),message_id=10202)
        report['answer']=dict(original_run=answer_run,controller_returns=430,exact_controller_words=count,
                              choice_states=430,exact_choice_words=1720,accepted_answers=accepts,
                              observed_pressed=pressed,exact_manager_samples=474,exact_manager_words=1896,
                              synchronous_bank_load_differences=gap,control_return=False,
                              narration=dict(message=10202,original_frame=471,native_frame=15697,
                                             controller_returns=9,exact_controller_words=words,
                                             limit='Independent message-opening alignment; native starts six frames earlier'))
    if a.narration:
        prefix='local/reference-village-return-narration-540-v1'
        reference_run=json.loads(Path(prefix+'.run.json').read_text())
        require(reference_run['exit_code']==0 and len(manager)>=16300,'Incomplete narration comparison')
        for suffix,sha in {
            '.controller.bin':'40b31bfd69671fcce7bc5d603d7abfb8d23b8e886d980f49cf1ba7515d2c3216',
            '.transition.bin':'1c06e8cb22c1ae707c34f1abac7315ce63b43adfd08125f48e62150a8033d07c',
            '.camera.bin':'9307531db9bf729abbcb885c52da360b6f8f6c60633c4e83fc45710bed2eebd3',
        }.items():
            f=Path(prefix+suffix);require(digest(f)==sha,'Changed original narration corpus')
            report['sha256'][str(f)]=sha
        count=compare_controller(Path(prefix+'.controller.bin').read_bytes(),native('.dialogue.bin').read_bytes(),
                                 first_original=0,first_native=15706,frames=518,total=len(manager),message_id=10202)
        managers=[r for r in rows(Path(prefix+'.transition.bin').read_bytes(),128) if r[1]==0x133a98 and r[0]<=520]
        require([r[0] for r in managers]==list(range(521)),'Narration manager extent')
        for r in managers:
            n=manager[15704+r[0]]
            require((n[4],n[7],n[5],n[6])==(int(bool(r[35]&256)),r[36],r[48],r[118]),'Narration manager differs')
        cameras=[r for r in rows(Path(prefix+'.camera.bin').read_bytes(),32) if r[0]<=519]
        require([r[0] for r in cameras]==list(range(520)),'Narration camera extent')
        for r in cameras:
            require(camera[15705+r[0]][4:16]==r[4:8]+r[12:16]+r[8:12],'Narration camera differs')
        for r in managers[:518]:
            require(actors[15710+r[0]][8:16]==r[40:48],'Pre-load-aligned HERO differs')
        mismatches=sum(actors[15704+r[0]][8:16]!=r[40:48] for r in managers)
        require(mismatches==521,'Changed narration-relative actor phase difference')
        report['narration_continuation']=dict(original_run=reference_run,controller_returns=518,
            exact_controller_words=count,message_aligned_manager_samples=521,exact_manager_words=2084,
            camera_updates=520,exact_camera_words=6240,pre_load_aligned_hero_samples=518,exact_hero_words=4144,
            message_aligned_hero_mismatches=521,
            limit='Six-frame loading gap changes actor phase relative to narration; two stated anchors do not establish integrated timing equality')
    write_exact(a.output,(json.dumps(report,indent=2)+'\n').encode())
    print('Village return entry exact; dialogue compared:',a.dialogue)

if __name__=='__main__':main()
