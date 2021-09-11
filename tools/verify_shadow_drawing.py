#!/usr/bin/env python3
"""Verify native effect24 geometry factories, draw transforms/materials and UV sharing.

Shape3 compares exact triangle multisets because retail strip optimization changes
order. Other geometry compares ordered vertices. No numerical tolerance is used.
This does not prove raster equivalence, global template-load RNG, or service54.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_map_effect import digest, ELF_SHA
from verify_summon_drawing import unpack_records

META=((25,0x290,0x21,0xffffffff),)*3+((3,0,0x21,0xffffffff),
      (55,0x800,0x21,0xffffffff),(31,0x441,0x121,17),
      (31,0x401,0x321,17),(31,0x401,0x321,17))
SHAPE_CASES=((3,1),(55,1),(55,0x1234),(3,0x1234),(55,0x7fffffff))

def shape_fixture():
    return b''.join(struct.pack('<3I',i,shape,seed) for i,(shape,seed) in enumerate(SHAPE_CASES))

def drawing_fixture():
    data=bytearray();identity=(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)
    for case in range(5):
        job=((0,0,0,1),(0,0,0,0),(1,1,1,1),(1,1,1,1)) if not case else (
            (3*case,-4*case,5*case,1),(.031*case,-.077*case,.019*case,0),(.5+case/4,1.5,2,1),(.4,.6,.8,.5))
        camera=(0,0,0,1) if not case else (.123*case,-.231*case,.02*case,1)
        view=identity if case<3 else (0,0,1,0,0,1,0,0,-1,0,0,0,7,-3,11,1)
        data+=struct.pack('<I37f',case,*(x for v in job for x in v),*camera,*view,450)
        for i,meta in enumerate(META):
            channels=((i*.2,-i*.4,5+i*.1,0),(.025*i,.03*i,-.07*i,0),(2.5,3.5,1,1),(.65,.55,.45,.75))
            data+=struct.pack('<16f4I',*(x for v in channels for x in v),*meta)
        data+=struct.pack('<90I',*((-192+j%30*3)&65535 for j in range(90)))
    return bytes(data)

class Reader:
    def __init__(self,data):self.data=data;self.at=0
    def take(self,n):
        if n<0 or len(self.data)-self.at<n:raise ValueError('Shadow geometry extent')
        p=self.data[self.at:self.at+n];self.at+=n;return p
    def words(self,n):return struct.unpack('<%dI'%n,self.take(n*4))
    def finish(self):
        if self.at!=len(self.data):raise ValueError('Trailing shadow geometry')

def packets(reader,pieces,textured):
    if not 1<=pieces<=2:raise ValueError('Shadow geometry piece count')
    triangles=[]
    for _ in range(pieces):
        parts=[]
        for kind in range(4):
            size=reader.words(1)[0]
            if kind==1 and not textured:
                if size:raise ValueError('Untextured shadow has UV data')
            elif not 64<=size<=8192 or size%16:raise ValueError('Shadow packet extent')
            parts.append(reader.take(size))
        pos,uv,color,indices=parts
        points=[v for c in unpack_records(pos,0x79,'<3h') for v in c]
        colors=[v for c in unpack_records(color,0x6e,'<4B') for v in c]
        coords=[v for c in unpack_records(uv,0x75,'<2H') for v in c] if textured else [(0,0)]
        for strip in unpack_records(indices,0x6e,'<4B'):
            if len(strip)<3:raise ValueError('Empty shadow strip')
            corners=[]
            for p,normal,c,t in strip:
                if normal or p>=len(points) or c>=len(colors) or t>=len(coords):raise ValueError('Shadow corner bounds')
                corners.append((*[x/64 for x in points[p]],1.,*colors[c],coords[t][0]/32768,coords[t][1]/32768,0.,0.))
            for i in range(len(corners)-2):triangles.extend(corners[i:i+3])
    return triangles

def compare_triangles(expected,actual,shape):
    wanted={3:672,55:240,25:180,31:6}[shape]
    if len(expected)!=wanted or len(actual)!=wanted:raise ValueError('Shadow triangle count')
    if shape==3:
        key=lambda v:Counter(tuple(sorted(v[i:i+3])) for i in range(0,len(v),3))
        same=key(expected)==key(actual)
    else:same=expected==actual
    if not same:raise ValueError(f'Shadow shape{shape} triangle values differ')
    return wanted

def compare_shapes(original,native):
    r=Reader(original);n=Reader(native);total=0
    for case,(shape,seed) in enumerate(SHAPE_CASES):
        identity,got_shape,got_seed,rng,pieces=r.words(5)
        header=n.words(5)
        if (identity,got_shape,got_seed)!=(case,shape,seed) or header[:4]!=(case,shape,seed,rng):
            raise ValueError('Shadow factory identity/RNG')
        if shape==3 and rng!=seed:raise ValueError('Sphere consumed RNG')
        expected=packets(r,pieces,False)
        if header[4]!={3:672,55:240}[shape]:raise ValueError('Shadow factory vertex count')
        actual=list(struct.iter_unpack('<12f',n.take(header[4]*48)))
        total+=compare_triangles(expected,actual,shape)
    r.finish();n.finish();return total

def compare_draws(original,native):
    r=Reader(original);n=Reader(native);total=0
    for i,(shape,_,_,texture) in enumerate(META):
        header=r.words(11);expected_uv=0xffffffff if texture==0xffffffff else 0x1f001f
        if header[:4]!=(i,shape,0,expected_uv) or header[4:10]!=(0,1,0x42,0,0x50000,0):
            raise ValueError('Shadow draw identity/UV/material')
        expected=packets(r,header[10],texture!=0xffffffff)
        element,count,got_texture=n.words(3)
        if (element,count,got_texture)!=(i,len(expected),texture):raise ValueError('Native shadow draw identity')
        actual=list(struct.iter_unpack('<12f',n.take(count*48)))
        total+=compare_triangles(expected,actual,shape)
    r.finish();n.finish();return total

def compare_uv(original,native):
    if len(original)!=84*3*7*4 or len(native)!=len(original):raise ValueError('Shadow UV extent')
    for index,row in enumerate(struct.iter_unpack('<7I',original)):
        if row[:3]!=(index//3,index%3+5,4):raise ValueError('Shadow UV identity')
    if original!=native:raise ValueError('Shadow shared/replaced UV mismatch')
    return 84*3*4

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--reference',type=Path,default=Path('local/research/effect24-v1'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);b=a.reference
    write_exact(b/'shapes-fixture-v1.bin',shape_fixture());write_exact(b/'geometry-fixture-v1.bin',drawing_fixture())
    if digest(Path('local/extracted/SCUS_971.29'))!=ELF_SHA:raise ValueError('Different retail ELF')
    runs=[]
    for name in ('shapes-original-v3','geometry-original-v1','24.original-v2'):
        run=json.loads((b/(name+'.run.json')).read_text());runs.append(run)
        if run['exit_code'] or run['elf_sha256']!=ELF_SHA:raise ValueError('Incomplete original corpus')
        for name,sha in run.get('outputs',{name+'.bin':run.get('output_sha256')}).items():
            if digest(b/name)!=sha:raise ValueError('Changed original corpus')
        if digest(Path(run['command'][-2 if run['command'][1]!='map-effect-probe' else -3]))!=run['input_sha256']:
            raise ValueError('Changed original fixture')
    common=['local/effect-input/CMNDATA/2D/TES.EFD','local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF']
    commands=[['effect-shape',str(b/'shapes-fixture-v1.bin'),str(a.output/'shapes.bin')],
              ['shadow24-effect-geometry',*common,str(b/'geometry-fixture-v1.bin'),str(a.output/'drawing.bin')],
              ['map-effect-probe',*common,str(b/'24.input.bin'),'82',str(a.output/'state.bin')]]
    for cmd in commands:subprocess.run([str(a.native),*cmd],check=True)
    factories=compare_shapes((b/'shapes-original-v3.bin').read_bytes(),(a.output/'shapes.bin').read_bytes())
    vertices=compare_draws((b/'geometry-original-v1.bin.packets.bin').read_bytes(),(a.output/'drawing.bin.vertices.bin').read_bytes())
    original=(b/'geometry-original-v1.bin').read_bytes();native=(a.output/'drawing.bin').read_bytes()
    if len(original)!=5*161*4 or native!=original:raise ValueError('Shadow transform/color mismatch')
    for i,row in enumerate(struct.iter_unpack('<161I',original)):
        if row[0]!=i:raise ValueError('Shadow transform identity')
    materials=struct.pack('<24I',*(v for i in range(8) for v in (i,2,1)))
    if (a.output/'drawing.bin.materials.bin').read_bytes()!=materials:raise ValueError('Native shadow material mismatch')
    uv=compare_uv((b/'24.original-v2.bin.shadow-uv.bin').read_bytes(),(a.output/'state.bin.shadow-uv.bin').read_bytes())
    if (b/'24.original-v2.bin').read_bytes()!=(a.output/'state.bin').read_bytes():raise ValueError('State regression')
    report=dict(scope=__doc__,original_runs=runs,native_commands=[[str(a.native),*c] for c in commands],
                native_sha256=digest(a.native),factory_vertices_exact=factories,draw_vertices_exact=vertices,
                transform_colors_exact=40,shared_uv_entries_exact=uv,
                sphere_comparison='Exact triangle multiset including multiplicity; order and winding differ. No raster equality claim.',
                native_material='Subtractive, depth-tested/no-write, no culling')
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(f'{factories} factory vertices, {vertices} draw vertices, 40 transforms/colors, {uv} UV entries exact')

if __name__=='__main__':main()
