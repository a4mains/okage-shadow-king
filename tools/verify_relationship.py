#!/usr/bin/env python3
"""Audit native LA metadata against bounded original handler/controller execution.

156 cases cover all three choice indices,13 reviewed adjustment codes, signed
clamping and overflow. Optional live comparison covers the first accepted answer
and its next240 frames; it does not establish conversation completion or pixels.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_message_opening import require, digest, rows, compare_controller

CASES_SHA='9fabcb192c036c58a402802a44ae042a96ca6ac132f07656a5d79c683b73ca17'
ORIGINAL_SHA='d8c1363a322c1d1f95b66d8050d6348286e49c6e0120e023cde56f4727290a64'
CONTROLLER_SHA='cef9b750c5a40aa29884fef76dfc416b04c4a820c1185df20c2c3851c7df8d71'
CHOICES_SHA='f00fc8474fb8f097d20f165dede7c8ea257b1993b349d2526e34bcbb51af8546'

def cases():
    result=[]
    for choice in range(3):
        for code in range(13):
            for values in [(0,1,2,3,4,5),(-2,-1,0,1,2,3),(0x7fffffff,)*6,(0x7ffffffe,)*6]:
                codes=[0,3,12];codes[choice]=code
                result.append(struct.pack('<10I',choice,*codes,*[v&0xffffffff for v in values]))
    return b''.join(result)

def compare(original,native):
    reference=rows(original,29);actual=rows(native,16)
    require(len(reference)==len(actual)==156,'Relationship case count')
    for i,(r,n) in enumerate(zip(reference,actual)):
        require(r[0]==11 and r[7:13]==(0x8910,9,0xfffffffe,7,5,255),'Original handler contract changed')
        require(r[1:7]==r[13:19],'Handler/controller variable results differ')
        require(r[19:]==(0x8000,0,1,0,5,4,0x50,5,255,i//52),'Original controller resume contract changed')
        require(r[13:]==n,f'Native relationship case{i} differs')
    return len(actual)*16

def live_answer(capture):
    ref=Path('local/reference-neighbor-answer-240-v1.controller.bin')
    choice=Path(str(ref)+'.choices.bin')
    require(digest(ref)==CONTROLLER_SHA and digest(choice)==CHOICES_SHA,'Changed original answer corpus')
    native=Path(str(capture)+'.dialogue.bin')
    native_choice=Path(str(capture)+'.choices.bin')
    run=json.loads(Path(str(capture).removesuffix('.png')+'.run.json').read_text())
    original_run=json.loads(Path('local/reference-neighbor-answer-240-v1.run.json').read_text())
    require(run['exit_code']==original_run['exit_code']==0,'Incomplete answer run')
    count=compare_controller(ref.read_bytes(),native.read_bytes(),first_original=0,first_native=6150,frames=240,total=6389)
    options=rows(native_choice.read_bytes(),12);reference=rows(choice.read_bytes(),16)
    require(len(options)==6389 and len(reference)==480,'Answer choice extent')
    for r in reference[1::2]:
        require(r[2:6]==options[6149+r[0]][2:6],f'Answer selection differs at{r[0]}')
    require(reference[21][5]==0 and options[6159][5]==0,'First answer must select option0')
    return dict(controller_words=count,choice_words=960,native_run=run,original_run=original_run,
                sha256={str(p):digest(p) for p in [ref,choice,native,native_choice,capture]})

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,default=Path('build/native/okage-assets'))
    p.add_argument('--capture',type=Path)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--prepare',action='store_true',help='Create reproducible input only; does not run original code')
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    fixture=args.output/'cases.bin';write_exact(fixture,cases())
    require(digest(fixture)==CASES_SHA,'Fixture generation changed')
    if args.prepare:return
    original=Path('local/relationship-v1/original.bin')
    require(digest(original)==ORIGINAL_SHA,'Changed original relationship corpus')
    run=json.loads(Path('local/relationship-v1/original.run.json').read_text())
    require(run['exit_code']==0,'Incomplete original relationship probe')
    native=args.output/'native.bin';cmd=[str(args.native),'relationship',str(fixture),str(native)]
    subprocess.run(cmd,check=True)
    report=dict(scope=__doc__,cases=156,exact_words=compare(original.read_bytes(),native.read_bytes()),
                original_run=run,native_command=cmd,sha256={str(p):digest(p) for p in [fixture,original,native,args.native]})
    if args.capture:report['live_answer']=live_answer(args.capture)
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(f"{report['exact_words']} relationship words match across156 cases")

if __name__=='__main__':main()
