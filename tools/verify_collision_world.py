#!/usr/bin/env python3
"""Compare native collision registry, recursive pushing and outer map retries."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
from verify_event import write_exact
from verify_map_contacts import room_asset

def fixtures(synthetic):
    rows=[]
    slope=struct.unpack('<f',struct.pack('<I',0x3efff17d))[0]
    def body(index,position,priority=10):
        return {'previous':position,'position':position,'point':(0,0,0,1),
                'first':(0,-2.5,0,1),'second':(0,-5,0,1),'radius':1.9,
                'broad':3.8,'reach':4,'flags':1,'priority':priority,'id':index,
                'point_flags':8,'capsule_flags':0x72,'registered':1}
    def add(bodies,destination,moving=0,bias=0):
        header=struct.pack('<6I6f',moving,3,0,8190,2,bias&0xffffffff,.5,slope,*destination,1)
        data=[]
        for b in bodies:
            data.append(struct.pack('<23f6I',*b['previous'],1,*b['position'],1,*b['point'],*b['first'],*b['second'],
                                    b['radius'],b['broad'],b['reach'],b['flags'],b['priority'],b['id'],
                                    b['point_flags'],b['capsule_flags'],b['registered']))
        rows.append(header+b''.join(data))
    y=6 if synthetic else 0
    for other_x in (1.,2.,3.,5.,7.):
        for priority in (0,10,20):
            for dx in (-.55,.55,2.5):
                bodies=[body(0,(0,y,4)),body(1,(other_x,y,4),priority),body(2,(7,y,4),20)]
                add(bodies,(dx,y,4))
                for key,value in [('flags',3),('registered',0),('id',0)]:
                    modified=copy.deepcopy(bodies);modified[1][key]=value;add(modified,(dx,y,4))
                modified=copy.deepcopy(bodies);modified[0]['flags']=0;add(modified,(dx,y,4))
                add(bodies,(dx,y,4),bias=1)
                add(bodies,(dx,y,4),bias=-1)
    rng=random.Random(0x186fa0)
    for _ in range(180):
        bodies=[body(i,(rng.uniform(-3,7),y,rng.uniform(0,8)),rng.choice((0,10,20))) for i in range(3)]
        moving=rng.randrange(3);position=bodies[moving]['position']
        destination=tuple(v+rng.uniform(-.8,.8) for v in position)
        add(bodies,destination,moving)
    return b''.join(rows)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,default=Path('build/native/okage-assets'))
    p.add_argument('--reference',type=Path)
    p.add_argument('--output',type=Path,default=Path('local/collision-world-validation'))
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    elf=Path('local/extracted/SCUS_971.29');oracle=Path('build/play-research/okage-oracle/okage-pose-oracle')
    digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
    if digest(elf)!='2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5':raise RuntimeError('Unexpected ELF')
    report={'scope':"Three-body registry, capsule dispatch, recursive pushing and outer map retries",
            'functions':['0x186fa0','0x187c60','0x187ea8'],'cases':[],
            'elf_sha256':digest(elf),'native_sha256':digest(args.native),
            'excluded':['other dynamic shape types','resource-driven registration','full original scene movement'],
            'preconditions':'Fresh synthetic body records, owned/synthetic XCL; unpatched original helpers; no captured RAM'}
    if not args.reference:report['oracle_sha256']=digest(oracle)
    for name in ('room','home1c'):
        asset=Path('local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF/a.xcl')
        if name=='room':asset=args.output/'room.xcl';write_exact(asset,room_asset())
        data=fixtures(name=='room');fixture=args.output/(name+'.queries.bin');write_exact(fixture,data)
        reference=(args.reference or args.output)/(name+'.retail.bin')
        if args.reference:
            if (args.reference/fixture.name).read_bytes()!=data:raise RuntimeError('Reference queries differ')
        else:subprocess.run([str(oracle),'collision-world',str(elf),str(asset),str(fixture),str(reference)],check=True)
        native=args.output/(name+'.native.bin')
        subprocess.run([str(args.native),'collision-world',str(asset),str(fixture),str(native)],check=True)
        expected,actual=reference.read_bytes(),native.read_bytes()
        if len(expected)!=len(data)//396*284 or len(actual)!=len(expected):raise RuntimeError('Unexpected result extent')
        diffs=[i//284 for i in range(0,len(expected),284) if expected[i:i+284]!=actual[i:i+284]]
        if diffs:raise RuntimeError(f'{name}: {len(diffs)} mismatched rows; first {diffs[:20]}')
        rows=list(struct.iter_unpack('<71I',expected))
        flags={str(flag):sum(row[5+body*22+4]==flag for row in rows for body in range(3))
               for flag in sorted({row[5+body*22+4] for row in rows for body in range(3)})}
        pushed=0
        for i,row in enumerate(rows):
            moving=struct.unpack_from('<I',data,i*396)[0]
            for body in range(3):
                if body==moving:continue
                old=struct.unpack_from('<4I',data,i*396+48+body*116+16)
                pushed+=tuple(row[5+body*22:9+body*22])!=old
        if not pushed:raise RuntimeError('No changed other-body positions')
        report['cases'].append({'name':name,'count':len(rows),'exact':True,'other_body_position_changes':pushed,
                                'body_flags':flags,'hashes':{str(path):digest(path) for path in (asset,fixture,reference,native)}})
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print('Original/native collision registry and pushing match exactly')

if __name__=='__main__':main()
