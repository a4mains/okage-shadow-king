#!/usr/bin/env python3
"""Compare native sky translation to three original saved render states.

Uses owned STG factors and captured original camera/sky channels. These are
12 exact words, not full renderer, camera trajectory, or pixel equivalence.
Reference RAM is an auditor input only, never a native application input.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_message_opening import require,digest

SNAPSHOTS=[
 ('neighbor251-v2-ee.bin','1fc9bb33762cbbd5a7cff8c3aafcb93af9650e1ec1696eacf8a79d81a5cfdae5'),
 ('neighbor-handoff300-v1-ee.bin','7cae10cbfca5fb6ae0646bfe768e20951f6f1c5d425627f257de3a704988fa1a'),
 ('field-path181-v2-ee.bin','815e4fdc7486ffca2f30c64d906966df27ee77a32bcc6aab450ac9997f884621')]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,default=Path('build/native/okage-assets'))
    p.add_argument('--capture',type=Path)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    stage=Path('local/xpf-decoded/FLDDATA/MAP/FIELD/FIELD_1D.XPF/a.stg')
    source=stage.read_bytes();word=lambda at:struct.unpack_from('<I',source,at)[0]
    require(word(0)==0x475453,'Invalid STG')
    factors=bytes.fromhex('0000003f000000000000003f00000000')
    for i in range(word(12)):
        at=word(8)+20*i
        if word(at)==0x46:factors=source[at+4:at+20]
    cameras=[];positions=[];files=[stage,args.native];snapshots=[]
    for name,sha in SNAPSHOTS:
        path=Path('local/research')/name
        require(digest(path)==sha,'Changed original sky snapshot');data=path.read_bytes()
        require(len(data)==32*1024*1024,'EE snapshot extent')
        word=lambda at:struct.unpack_from('<I',data,at)[0]
        require(word(0x337b68)==0 and data[0x337e50:0x337e60]==factors,'Original sky instance/factors differ')
        object=word(0x21301c);sky=word(0x33760c)+((word(object+4)&0xffff)+1)*0x90+0x20
        camera=word(0x33760c)+word(0x337d8c)*0x90+0x20
        require(0<sky<=len(data)-16 and 0<camera<=len(data)-16,'Sky/camera channel bounds')
        cameras.append(data[camera:camera+16]);positions.append(data[sky:sky+16]);files.append(path)
        snapshots.append(dict(ram=str(path),sky_instance=0,camera_channel=camera,sky_channel=sky))
    input_path=args.output/'camera.bin';output_path=args.output/'native.bin'
    write_exact(input_path,b''.join(cameras));write_exact(args.output/'original.bin',b''.join(positions))
    command=[str(args.native),'sky-follow',str(stage),str(input_path),str(output_path)]
    subprocess.run(command,check=True)
    require(output_path.read_bytes()==b''.join(positions),'Native sky position differs')
    files.extend([input_path,output_path,args.output/'original.bin'])
    report=dict(scope=__doc__,exact_words=12,snapshots=snapshots,native_command=command,
                sha256={str(path):digest(path) for path in files})
    if args.capture:
        base=str(args.capture);run=json.loads(Path(base.removesuffix('.png')+'.run.json').read_text())
        require(run['exit_code']==0,'Incomplete native path capture')
        original_run=json.loads(Path('local/reference-field-path-181-v2.run.json').read_text())
        require(original_run['exit_code']==0,'Incomplete original path reference')
        before='local/native-field-path-7410-relationship-v2.png'
        suffixes=['.dialogue.bin','.choices.bin','.interaction.bin','.movement.bin','.camera.bin','.actor-1.bin','.variables.bin']
        for suffix in suffixes:
            path=Path(base+suffix);previous=Path(before+suffix)
            require(path.read_bytes()==previous.read_bytes(),'Sky change altered native simulation: '+suffix)
            report['sha256'][str(path)]=digest(path);report['sha256'][str(previous)]=digest(previous)
        report['capture']=dict(native_run=run,original_run=original_run,unchanged_state_traces=suffixes,
                               visual_scope='Image must be inspected; no automated pixel equivalence claim')
        report['sha256'][base]=digest(args.capture)
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print('12 original sky-position words match')

if __name__=='__main__':main()
