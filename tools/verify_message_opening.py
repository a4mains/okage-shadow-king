#!/usr/bin/env python3
"""Audit message10001 logical initialization,120 live controller frames and camera endpoint.

Reuses completed original evidence. Excludes general cold initialization, RUW
geometry/pixels, other message types, full conversation completion and audio.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact

INIT_SHA='76af1db78a898664294c8c6ace5d67031c6c1a085ad973894a69db5c541bf4c2'
RAM_SHA='1fc9bb33762cbbd5a7cff8c3aafcb93af9650e1ec1696eacf8a79d81a5cfdae5'
CONTROLLER_SHA='408465d3fbbf3c0ce95d8eb051158472c1fb42f41aed052aef0c4fe0075902c1'

def require(condition,why):
    if not condition:raise ValueError(why)

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def rows(data,width):
    require(bool(data) and len(data)%(width*4)==0,'Trace extent')
    return list(struct.iter_unpack('<'+str(width)+'I',data))

def initialization_projection(data, message_id=10001):
    stride=0xef0+2*0x6ec0
    require(len(data)==3*stride,'Initialization snapshot extent')
    result=[]
    for phase in range(3):
        word=lambda at:struct.unpack_from('<I',data,phase*stride+at)[0]
        require(word(0x8c)==message_id and word(0x16c)==message_id and word(0xe40)==2,'Message identity/layers')
        require(word(0x88)==0 and word(0x168)==0,'Unreviewed message bank')
        body=0xef0+0x6ec0
        row=[word(a) for a in [0xe4c,0x90,0x170,0xed8,0xed4]]
        row += [word(body+a) for a in [0x1900,0x20c,0x21c,0x22c,0xcd0,0xcd4,0xcd8,0x3d08,
                                      0xcb0,0xcb4,0x1968,0xce8,0x260,0x264,0x268,0x26c,0x4f8]]
        for layer in [1,0]:row += [word(0xef0+layer*0x6ec0+a) for a in [0x270,0x274,0x278,0x27c]]
        require(row[0]==(2 if phase==2 else 0) and row[1:5]==[0x50,0x50,0xffffffff,0],
                'Initialization stage/readiness')
        result.extend(row)
    return struct.pack('<90I',*result)

def compare_controller(original,native,*,first_original=131,first_native=5730,frames=120,total=5850,message_id=10001):
    reference=rows(original,64);live=rows(native,12)
    require(len(reference)==frames*2 and len(live)==total,'Controller interval extent')
    require([r[0] for r in live]==list(range(1,total+1)),'Native frame identity')
    for i in range(frames):
        before,after=reference[2*i:2*i+2]
        require(before[:2]==(first_original+i,0x148dc8) and after[:2]==(first_original+i,0x11ff38),'Controller pairing')
        n=live[first_native-1+i]
        expected=(after[32],after[33],after[34],after[35],after[36],after[16],after[21],after[17],after[7],after[43])
        require(n[1]==message_id and n[2:]==expected,f'Controller state differs at original{first_original+i}')
    return frames*10

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,default=Path('build/native/okage-assets'))
    p.add_argument('--capture',type=Path,default=Path('local/native-outdoor-neighbor-5850-message-v2.png'))
    p.add_argument('--choice-capture',type=Path,help='Optional completed6149-frame continuation to the first choice')
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    original=Path('local/message-init-v2/10001.original.bin')
    ram=Path('local/research/neighbor251-v2-ee.bin')
    controller=Path('local/reference-outdoor-neighbor-251-v2.controller.bin')
    for path,sha in [(original,INIT_SHA),(ram,RAM_SHA),(controller,CONTROLLER_SHA)]:
        require(digest(path)==sha,'Changed reviewed original corpus')
    init_run=json.loads(Path('local/message-init-v2/original.run.json').read_text())
    reference_run=json.loads(Path('local/reference-outdoor-neighbor-251-v2.run.json').read_text())
    native_run=json.loads(Path(str(args.capture).removesuffix('.png')+'.run.json').read_text())
    require(all(r['exit_code']==0 for r in [init_run,reference_run,native_run]),'Incomplete original/native run')
    output=args.output/'initialization.bin'
    command=[str(args.native),'field-message-init','local/text-input/FLDDATA/MES/FIELD.PME','10001',str(output)]
    subprocess.run(command,check=True)
    require(output.read_bytes()==initialization_projection(original.read_bytes()),'Native initialization differs')
    trace=Path(str(args.capture)+'.dialogue.bin')
    count=compare_controller(controller.read_bytes(),trace.read_bytes())
    interaction=rows(Path('local/reference-outdoor-neighbor-251-v2.interaction.bin').read_bytes(),176)
    require([(r[0],r[6]) for r in interaction if r[1]==0x131cac]==[(131,6)],'Original NPC selection differs')
    require(any(r[0]==131 and r[1]==0x1bb7e0 and r[3]==61 for r in interaction),'Original event61 missing')
    native_interactions=rows(Path(str(args.capture)+'.interaction.bin').read_bytes(),8)
    require(native_interactions[5729][:6]==(5730,0x40,0xffffffeb,61,1,1),'Native event selection differs')
    movement=rows(Path(str(args.capture)+'.movement.bin').read_bytes(),16)
    for before in rows(controller.read_bytes(),64)[::2]:
        index=before[0]+5598
        require(native_interactions[index][1]==before[9] and movement[index][1]==before[8],'Pad latch differs')
    data=ram.read_bytes();word=lambda at:struct.unpack_from('<I',data,at)[0]
    message=word(word(0x2d9bf4)+0x50)
    require(word(message+0x8c)==10001 and word(0x1fb970)==0xffffffff,'Reference message/camera mode')
    channel=word(0x33760c)+word(0x337d8c)*0x90
    endpoint=tuple(word(a+i*4) for a in [channel+0x20,0x337db0,channel+0xb0] for i in range(4))
    camera=rows(Path(str(args.capture)+'.camera.bin').read_bytes(),16)
    require(len(camera)==5850 and camera[-1][4:]==endpoint,'Native camera endpoint differs')
    require(all(r[4:]==endpoint for r in camera[5729:]),'Native scripted camera drifted')
    files=[original,ram,controller,trace,args.capture,output,Path(str(args.capture)+'.camera.bin'),
           Path(str(args.capture)+'.interaction.bin'),Path(str(args.capture)+'.movement.bin')]
    report=dict(scope=__doc__,initialization_words=90,controller_frames=120,controller_words=count,
                camera_endpoint_words=12,native_camera_stable=True,init_run=init_run,reference_run=reference_run,
                native_run=native_run,initialization_command=command,native_tool_sha256=digest(args.native),
                sha256={str(path):digest(path) for path in files})
    if args.choice_capture:
        original_choice=Path('local/reference-neighbor-choice-300-v1.controller.bin')
        choice_trace=Path(str(args.choice_capture)+'.dialogue.bin')
        choice_run=json.loads(Path(str(args.choice_capture).removesuffix('.png')+'.run.json').read_text())
        original_choice_run=json.loads(Path('local/reference-neighbor-choice-300-v1.run.json').read_text())
        require(choice_run['exit_code']==original_choice_run['exit_code']==0,'Incomplete choice continuation')
        words=compare_controller(original_choice.read_bytes(),choice_trace.read_bytes(),
                                 first_original=0,first_native=5850,frames=300,total=6149)
        original_options=Path(str(original_choice)+'.choices.bin')
        native_options=Path(str(args.choice_capture)+'.choices.bin')
        options=rows(native_options.read_bytes(),12)
        reference_options=rows(original_options.read_bytes(),16)
        require(len(options)==6149 and len(reference_options)==600,'Choice trace interval')
        for r in reference_options[1::2]:
            require(r[2:6]==options[5849+r[0]][2:6],'Choice cursor/fragment/result differs')
        require(options[-1][6:8]==(1,0),'Native did not reach unarmed first choice')
        report['first_choice']=dict(controller_words=words,choice_words=1200,native_run=choice_run,
                                    original_run=original_choice_run)
        for path in [original_choice,choice_trace,original_options,native_options,args.choice_capture]:
            report['sha256'][str(path)]=digest(path)
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print('90 initialization words,120 controller frames and scripted camera endpoint match')

if __name__=='__main__':main()
