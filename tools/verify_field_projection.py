#!/usr/bin/env python3
"""Compare native field/effect projection lifecycle with the six-frame observer.

This corpus proves stable basement settings, not cold initialization, varying
effect clipping, rendering depth, shadow submission or whole-route fidelity.
Setter observations are at function entry: apply their recorded arguments only
where the next snapshot is after that setter. Never feed these data to the game.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from verify_event import write_exact

NAMES=('shadow','ordinary','field_end','effects','deferred','ending')
FIELDS=('center_x','center_y','half_width','half_height','depth_near','depth_far',
        'clip_near','clip_far','depth_min','depth_max','fog_near','fog_far','scale_x','scale_y','focal')

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def reference_frames(data):
    if not data or len(data)%1200:raise ValueError('Original projection extent')
    rows=list(struct.iter_unpack('<300I',data))
    groups={}
    for row in rows:groups.setdefault(row[0],[]).append(row)
    if list(groups)!=list(range(len(groups))):raise ValueError('Original frame identities')
    expected=[0x1b9be0,0x1b9288,0x183f18,0x183df0]+[0x185f20,0x1b8dc8]*7+[
        0x183f18,0x183df0,0x102218,0x183f18,0x183df0,0x102cf8,0x183df0]
    result=[]
    for frame,group in groups.items():
        if [r[1] for r in group]!=expected:raise ValueError('Uncovered original submission sequence')
        near=group[-1][131]
        for row in group:
            if row[131]!=near:raise ValueError('Uncovered effect near mutation')
            for i in range(84):
                flags,job_near=row[132+i*2:134+i*2]
                if flags!=0xffffffff and job_near!=near:raise ValueError('Uncovered job clipping plane')
        values=[list(group[i][16:31]) for i in (1,4,20,22,24,24)]
        for i in range(4,18,2):
            if list(group[i][16:31])!=values[1]:raise ValueError('Ordinary settings changed within field')
        if group[22][2]!=0x102350 or group[24][2]!=0x102dc8:
            raise ValueError('Unexpected fog setter caller')
        values[3][10:12]=group[22][7:9]
        values[4][10:12]=values[5][10:12]=group[24][7:9]
        result.append((frame,near,values))
    return result

def compare(original,native,native_start,scene):
    if native_start<1:raise ValueError('Native anchor must be explicit and positive')
    expected=reference_frames(original)
    if not native or len(native)%376:raise ValueError('Native projection extent')
    rows=list(struct.iter_unpack('<94I',native))
    if [r[0] for r in rows]!=list(range(1,len(rows)+1)):raise ValueError('Native frame identities')
    differences=[];serial=0
    for row in rows:
        if row[2]<=serial:raise ValueError('Native projection did not advance')
        serial=row[2]
    for frame,near,values in expected:
        index=native_start+frame-1
        if index>=len(rows):raise ValueError('Missing native comparison frame')
        row=rows[index]
        if row[1]!=scene:raise ValueError('Wrong native scene at fixed anchor')
        if row[3]!=near:differences.append(dict(frame=row[0],field='effect_near',original=near,native=row[3]))
        for stage,(name,wanted) in enumerate(zip(NAMES,values)):
            actual=row[4+stage*15:19+stage*15]
            for field,a,b in zip(FIELDS,wanted,actual):
                if a!=b:differences.append(dict(frame=row[0],stage=name,field=field,original=a,native=b))
    return dict(native_frames=len(rows),native_projection_submissions=serial,
        original_frames=len(expected),native_start=native_start,scene=scene,
        compared_words=len(expected)*91,differences=differences)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--original',type=Path,required=True)
    p.add_argument('--native-prefix',type=Path,required=True)
    p.add_argument('--native-start',type=int,required=True)
    p.add_argument('--scene',type=int,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();reference_run=a.original.with_suffix('.run.json')
    native_run=Path(str(a.native_prefix)+'.run.json')
    native=Path(str(a.native_prefix)+'.png.projection.bin')
    r=json.loads(reference_run.read_text());n=json.loads(native_run.read_text())
    if r['exit_code'] or r['output_sha256']!=digest(a.original):raise ValueError('Incomplete/changed original observation')
    if n['exit_code']:raise ValueError('Native route did not complete')
    report=compare(a.original.read_bytes(),native.read_bytes(),a.native_start,a.scene)
    report.update(scope=__doc__,original_run=r,native_run=n,original_sha256=digest(a.original),native_sha256=digest(native))
    write_exact(a.output,(json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps({k:v for k,v in report.items() if not k.endswith('_run') and k!='scope'}))
    if report['differences']:raise ValueError('Native projection differs; retained report')

if __name__=='__main__':main()
