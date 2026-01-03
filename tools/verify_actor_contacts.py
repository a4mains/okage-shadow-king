#!/usr/bin/env python3
"""Compare native capsule-pair contacts against unpatched original execution."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
from verify_event import write_exact

def fixtures():
    rows = []
    threshold = struct.unpack('<f', struct.pack('<I', 0x3efff17d))[0]
    upright = (0,-2.5,0,1, 0,-5,0,1, 1.9)
    shapes = [upright, (0,-2.5,0,1, 1,-5,0,1, 1.9),
              (0,-2.5,0,1, 0,-5,1,1, 1.2),
              (0,-2.5,0,1, 2,-2.5,0,1, 1.9),
              (0,-2.5,0,1, .1,-5,.1,1, .5)]
    def add(old, new, moving=upright, other=upright, position=(0,0,0), reach=4, slope=threshold):
        rows.append((*old,1,*new,1,*position,1,*moving,*other,reach,slope))
    for moving, other in [(upright,s) for s in shapes]+[(s,upright) for s in shapes[1:]]:
        for x in (-4.,-2.,0.,.1,2.,4.):
            for y in (-8.,-5.,-3.,0.,3.,5.,8.):
                for z in (0.,2.):
                    add((x-.55,y,z),(x,y,z),moving,other)
                    add((x,y-1,z),(x,y,z),moving,other)
    rng = random.Random(0x18dbf8)
    for _ in range(400):
        point = tuple(rng.uniform(-6,6) for _ in range(3))
        old = tuple(v+rng.uniform(-1,1) for v in point)
        add(old,point,rng.choice(shapes),rng.choice(shapes),
            position=tuple(rng.uniform(-2,2) for _ in range(3)))
    for old,new in [((0,-5,0),(0,-4,0)),((0,5,0),(0,2,0)),((2,0,0),(2,0,0)),
                    ((0,0,0),(0,0,0)),((4,0,0),(3,0,0))]:
        for reach in (0.,4.):
            for slope in (0.,threshold,1.):
                add(old,new,reach=reach,slope=slope)
    return b''.join(struct.pack('<32f',*r) for r in rows)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native',type=Path,default=Path('build/native/okage-assets'))
    parser.add_argument('--reference',type=Path)
    parser.add_argument('--output',type=Path,default=Path('local/actor-contact-validation'))
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    elf=Path('local/extracted/SCUS_971.29');oracle=Path('build/play-research/okage-oracle/okage-pose-oracle')
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    if digest(elf)!='2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5':
        raise RuntimeError('Unexpected retail ELF')
    fixture=args.output/'queries.bin';data=fixtures();write_exact(fixture,data)
    reference=(args.reference or args.output)/'retail.bin'
    if args.reference:
        if (args.reference/'queries.bin').read_bytes()!=data:raise RuntimeError('Different reference queries')
    else:
        subprocess.run([str(oracle),'capsule-pair',str(elf),str(fixture),str(reference)],check=True)
    native=args.output/'native.bin'
    subprocess.run([str(args.native),'capsule-pair',str(fixture),str(native)],check=True)
    expected,actual=reference.read_bytes(),native.read_bytes()
    if len(expected)!=len(data)//128*56 or len(actual)!=len(expected):raise RuntimeError('Unexpected result extent')
    diffs=[i//56 for i in range(0,len(expected),56) if expected[i:i+56]!=actual[i:i+56]]
    if diffs:raise RuntimeError(f'{len(diffs)} mismatched rows; first {diffs[:20]}')
    rows=list(struct.iter_unpack('<14I',expected))
    counts={str(flag):sum(r[0] and r[1]==flag for r in rows) for flag in (0,1,2,4)}
    if not all(counts.values()) or not any(not r[0] for r in rows):raise RuntimeError('Missing contact/rejection coverage')
    report={'scope':'Capsule-pair helper hit, flags, correction XYZ and defined contact normals',
            'functions':['0x18dbf8','0x18d098'],'cases':len(rows),'exact':True,'hits_by_flags':counts,
            'excluded':['correction W and normal W','stale failure fields','shape dispatch/registry/pushing','app movement'],
            'preconditions':'Synthetic capsule/record inputs; original helpers; no captured RAM, OS services or VU microprogram',
            'hashes':{str(p):digest(p) for p in (elf,args.native,fixture,reference,native)}}
    if not args.reference:report['oracle_sha256']=digest(oracle)
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print('Original/native capsule-pair contacts match exactly')

if __name__=='__main__':main()
