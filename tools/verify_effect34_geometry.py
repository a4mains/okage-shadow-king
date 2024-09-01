#!/usr/bin/env python3
"""Audit effect34's original updates, mutable quads and bounded draw fixtures.

Reference RAM/packed geometry are diagnostic inputs only, never runtime assets.
Checks all14 elements/450updates and independently executed draw blocks. Excludes
raster pixels, whole-scene scheduling, authentic startup RNG and physical output.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_map_effect import ELF_SHA, compare

STATE_SHA = '61251f987d6519892366f86254d24ac498387065acd373d64c3da3aa79e7de75'
QUADS_SHA = '14f05fe9482a20eecb92b389363c1c7566972ef717e74f53a4e9eb7269995cee'
ALIASES_SHA = '2909aa06ac1a921508fdcda544ae53c2fb853a35fd1f1207040d7eb130983180'
STORAGE_SHA = 'f1c5b586a962bf3689b84a2b4baaf8931e85a19911f2fddd8d17bfca1979d7b6'
INPUT_SHA = '769c8ec963c03f97f47d6a38fe15e6edb88dd5d88a812f596f0496de3be3aa10'
TICKS = 450
ELEMENTS = 14
CASE_WORDS = 526

def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def rows(data, words):
    if not data or len(data) % (words * 4):
        raise ValueError('Incomplete word records')
    return list(struct.iter_unpack(f'<{words}I', data))

def quad_rows(data, count=452):
    out = rows(data, 197)
    if len(out) != count:
        raise ValueError('Incomplete quad interval')
    for step, row in enumerate(out):
        if row[0] != step:
            raise ValueError('Quad step identity/order')
        for i in range(ELEMENTS):
            at = 1 + 14*i
            if row[at:at+2] != (i, 28 if i < 3 else 31):
                raise ValueError('Quad element identity/order')
            if any(row[at+3+v*3] > 65535 for v in range(4)):
                raise ValueError('Quad Z includes unrelated heap bits')
    return out

def compare_quads(original, native):
    source = quad_rows(original)
    quad_rows(native)
    if original != native:
        a = struct.unpack(f'<{len(original)//4}I', original)
        b = struct.unpack(f'<{len(native)//4}I', native)
        at = next(i for i, (x, y) in enumerate(zip(a, b)) if x != y)
        raise ValueError(f'Packed quad mismatch record{at//197} word{at%197}')
    return {'records': len(source), 'elements_per_record': 14,
            'packed_xyz_uv_words_exact': 452*14*12}

def check_aliases(data):
    a = rows(data, 15)
    if len(a) != 452:
        raise ValueError('Incomplete alias interval')
    for step, row in enumerate(a):
        expected = (0,1,2,3,3,3,6) + ((3,)*7 if step < 2 else (6,)*7)
        if row != (step, *expected):
            raise ValueError('Original geometry sharing differs')
    return 'Elements7..13 (IDs15..21) share element6 (ID14) after first update;3..5 retain shared factory geometry'

def check_storage(data, quads):
    source = rows(data, 197)
    q = quad_rows(quads)
    if len(source) != 452:
        raise ValueError('Incomplete storage interval')
    template_variants = set()
    for step, row in enumerate(source):
        if row[0] != step:
            raise ValueError('Storage step identity')
        entries = [row[1+14*i:15+14*i] for i in range(14)]
        for i, entry in enumerate(entries):
            if any(x == 0 or x >= 32*1024*1024 for x in entry[:6]):
                raise ValueError('Storage pointer outside source RAM')
            if any(entry[7+v*2] > 65535 for v in range(4)):
                raise ValueError('Template Z includes unrelated heap bits')
        if len({e[0] for e in entries[:3]}) != 3 or len({e[2] for e in entries[:3]}) != 3:
            raise ValueError('Flame geometry/UV must be independently cloned')
        xyz = entries[0][1]
        for i, entry in enumerate(entries[:3]):
            if entry[1] != xyz or entry[4] != xyz or entry[2] == entry[5]:
                raise ValueError('Shared template XYZ/independent UV relation differs')
            positions = tuple(x for v in range(4) for x in q[step][3+14*i+v*3:5+14*i+v*3])
            if entry[6:] != positions:
                raise ValueError('Live XYZ no longer aliases template coordinates')
        template_variants.add(entries[0][6:])
    if len(template_variants) != 2:
        raise ValueError('Missing actual template mutation coverage')
    return 'All3 flame XYZ buffers alias mutable template; UV buffers cloned separately; both template flip states observed'

def definition(efd):
    def word(p):
        if p+4 > len(efd):
            raise ValueError('EFD extent')
        return struct.unpack_from('<I', efd, p)[0]
    if word(0) != 0x20444645 or word(4) > 128:
        raise ValueError('EFD header')
    p = 16
    for _ in range(word(4)):
        n = word(p+16)
        if n > 128 or p+48+n*116 > len(efd):
            raise ValueError('EFD element extent')
        if word(p) == 34:
            if n != 14:
                raise ValueError('Effect34 count')
            return [struct.unpack_from('<9I', efd, p+48+i*116) for i in range(n)]
        p += 48+n*116
    raise ValueError('Missing effect34')

def make_cases(state, quads, efd):
    if hashlib.sha256(state).hexdigest() != STATE_SHA or hashlib.sha256(quads).hexdigest() != QUADS_SHA:
        raise ValueError('Unreviewed original update fixture')
    a, q, definitions = rows(state, 645), quad_rows(quads), definition(efd)
    selected = {0,1,2,3,12,13,14,152,153,292,293,432,433,450,451}
    for i in range(14):
        seen = set()
        for step, row in enumerate(q):
            key = row[1+14*i:15+14*i]
            if key not in seen:
                seen.add(key); selected.add(step)
    selected = sorted(selected)
    if len(selected) > 64:
        raise ValueError('Draw fixture bound')
    identity = (1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)
    output = bytearray()
    for case, step in enumerate(selected):
        r = a[step]
        transforms = [((0,0,10,1),(0,0,0,0),(1,1,1,1),(0,0,0,0),identity),
                      ((3,-5,425,1),(.125,.25,-.125,0),(2,3,.5,1),(.1,.2,.3,0),identity),
                      ((3,5,8,1),(.33,.21,.17,0),(1.5,.25,4,1),(.24,.47,.13,0),
                       (0,0,-1,0,0,1,0,0,1,0,0,0,-2,-3,440,1))]
        position, rotation, scale, camera, view = transforms[case % 3]
        output.extend(struct.pack('<I37f', case, *position,*rotation,*scale,.75,.25,.5,.6,*camera,*view,450))
        for i, d in enumerate(definitions):
            at = 29+44*i
            output.extend(struct.pack('<16I4I', *r[at+24:at+40],d[2],r[at+20],d[3],r[at+40]))
        for i in range(14):
            output.extend(struct.pack('<12I', *q[step][3+14*i:15+14*i]))
        output.extend(struct.pack('<12f', .6,.8,.4,1, .2,.1,.3,.8, .25,.5,.75,.9))
        for i in range(14):
            output.extend(struct.pack('<2I', *r[29+44*i+41:29+44*i+43]))
    assert len(output) == len(selected)*CASE_WORDS*4
    return bytes(output), selected

def compare_draws(original, native, cases):
    a, b = rows(original, 281), rows(native, 281)
    if len(a) != cases or len(b) != cases:
        raise ValueError('Draw case coverage')
    differences=[]
    maximum=0.0
    for case,(x,y) in enumerate(zip(a,b)):
        if x[0]!=case or y[0]!=case:
            raise ValueError('Draw identity/order')
        for k,(u,v) in enumerate(zip(x[1:],y[1:])):
            if u==v:continue
            fu,fv=(struct.unpack('<f',struct.pack('<I',z))[0]for z in(u,v))
            delta=abs(fu-fv)
            maximum=max(maximum,delta) if math.isfinite(delta) else math.inf
            differences.append((case,k+1,hex(u),hex(v)))
    if differences:
        raise ValueError(f'Exact draw mismatch: {len(differences)} words, first{differences[0]}, maxabs{maximum}; no tolerance')
    return 0.0

def packet_cases(data, cases):
    """Decode original finalized compiler packets; retain real corner topology."""
    at = 0
    def take(n):
        nonlocal at
        if n > len(data)-at:
            raise ValueError('Packet extent')
        result = data[at:at+n]; at += n
        return result
    def words(n):
        return struct.unpack(f'<{n}I',take(n*4))
    result = []
    for case in range(cases):
        if words(1) != (case,):
            raise ValueError('Packet case identity')
        elements = []
        for i in range(14):
            idx,shape,uvmin,uvmax,zlo,zhi,alo,ahi,tlo,thi = words(10)
            if (idx,shape) != (i,28 if i<3 else 31):
                raise ValueError('Packet element identity')
            if (zlo,zhi,alo,ahi,tlo,thi) != (0,1,0x48 if i<3 else 0x44,0,0x50000 if i<3 else 0x5000d,0):
                raise ValueError('Unreviewed effect34 GS material')
            packets = []
            for _ in range(4):
                size, = words(1)
                if size < 64 or size > 8192 or size % 16:
                    raise ValueError('Packet size')
                packets.append(take(size))
            pos,uv,color,indices = packets
            word = lambda p,k: struct.unpack_from('<I',p,k)[0]
            if (word(pos,28),word(uv,28),word(color,20),(word(indices,44)>>16)&255) != (4,4,1,4):
                raise ValueError('Packet count')
            if (word(pos,44),word(uv,44),word(color,36),word(indices,44)) != (0x79048001,0x750441aa,0x6e014149,0x6e04c001):
                raise ValueError('Packet UNPACK')
            if len(pos)<72 or len(uv)<64 or len(color)<44 or len(indices)<64:
                raise ValueError('Packet payload extent')
            ps=list(struct.iter_unpack('<3h',pos[48:72]));us=list(struct.iter_unpack('<2H',uv[48:64]));cs=list(struct.iter_unpack('<4B',color[40:44]));corners=list(struct.iter_unpack('<4B',indices[48:64]))
            triangles=[]
            for n in range(2):
                for p,normal,c,t in corners[n:n+3]:
                    if normal or p>=4 or c>=1 or t>=4:
                        raise ValueError('Packet corner bounds')
                    triangles.append((*[v/64 for v in ps[p]],1.,*cs[c],us[t][0]/32768,us[t][1]/32768,0.,0.))
            elements.append((triangles,uvmin,uvmax))
        result.append(elements)
    if at != len(data):
        raise ValueError('Trailing packets')
    return result

def compare_vertices(packets, native, materials, fixture):
    f=rows(fixture,CASE_WORDS);expected=packet_cases(packets,len(f));at=mt=0;total=0
    for case,elements in enumerate(expected):
        if len(native)-at<4 or struct.unpack_from('<I',native,at)[0]!=case or len(materials)-mt<4 or struct.unpack_from('<I',materials,mt)[0]!=case:
            raise ValueError('Native vertex/material case identity')
        at+=4;mt+=4
        for i,(triangles,uvmin,uvmax) in enumerate(elements):
            if len(native)-at<12 or len(materials)-mt<12:
                raise ValueError('Native vertex/material header extent')
            index,count,texture=struct.unpack_from('<3I',native,at);at+=12
            material=struct.unpack_from('<3I',materials,mt);mt+=12
            if (index,count,texture)!=(i,6,f[case][38+i*20+19]) or material!=(i,int(i<3),1):
                raise ValueError('Native material/texture identity')
            if (uvmin,uvmax)!=f[case][498+i*2:500+i*2]:
                raise ValueError('Source UV descriptor differs from fixture')
            if len(native)-at<count*48:
                raise ValueError('Native vertex extent')
            actual=list(struct.iter_unpack('<12f',native[at:at+count*48]));at+=count*48
            if actual!=triangles:
                raise ValueError(f'Native topology/XYZ/UV/color differs case{case} element{i}')
            total+=count
    if at!=len(native) or mt!=len(materials):
        raise ValueError('Trailing native vertices/materials')
    return total

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path)
    p.add_argument('--reference',type=Path,required=True)
    p.add_argument('--draw-reference',type=Path)
    p.add_argument('--efd',type=Path,default=Path('local/effect-input/CMNDATA/2D/TES.EFD'))
    p.add_argument('--textures',type=Path,default=Path('local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF'))
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--prepare',action='store_true')
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    ref=args.reference;state=ref/'original.bin';quads=Path(str(state)+'.effect34-quads.bin');aliases=Path(str(state)+'.effect34-aliases.bin')
    run=json.loads((ref/'run.json').read_text())
    if run['exit_code'] or [digest(x) for x in (state,quads,aliases)] != [STATE_SHA,QUADS_SHA,ALIASES_SHA]:
        raise ValueError('Incomplete or changed original update evidence')
    storage=Path(str(state)+'.effect34-storage.bin')
    if digest(storage)!=STORAGE_SHA:raise ValueError('Changed original storage evidence')
    storage_scope=check_storage(storage.read_bytes(),quads.read_bytes())
    share=check_aliases(aliases.read_bytes());fixture,steps=make_cases(state.read_bytes(),quads.read_bytes(),args.efd.read_bytes())
    if args.prepare:
        write_exact(args.output/'cases.bin',fixture)
        write_exact(args.output/'cases.json',(json.dumps({'source_records':steps,'words_per_case':CASE_WORDS,'fixture_sha256':hashlib.sha256(fixture).hexdigest(),'source_state_sha256':STATE_SHA,'source_quads_sha256':QUADS_SHA,'lighting':'explicit base/ambient/light0;183d68/183d30/183ce0→1020e0 vs effect_light_tint','scope':__doc__},indent=2)+'\n').encode());return
    if not args.native:p.error('--native required for comparison')
    commands=[]
    def execute(argv,name):
        r=subprocess.run(argv,capture_output=True);write_exact(args.output/(name+'.log'),r.stdout+r.stderr)
        commands.append({'command':argv,'exit_code':r.returncode});write_exact(args.output/(name+'.run.json'),(json.dumps(commands[-1],indent=2)+'\n').encode());r.check_returncode()
    output=args.output/'state.bin';input_path=ref/'input.bin'
    if digest(input_path)!=INPUT_SHA:raise ValueError('Changed update input')
    execute([str(args.native),'map-effect-probe',str(args.efd),str(args.textures),str(input_path),'450',str(output)],'state')
    compare(state.read_bytes(),output.read_bytes(),34,14,450)
    quad_result=compare_quads(quads.read_bytes(),Path(str(output)+'.effect34-quads.bin').read_bytes())
    draw=args.draw_reference or ref/'draw';manifest=json.loads((draw/'run.json').read_text())
    if manifest['exit_code'] or (draw/'cases.bin').read_bytes()!=fixture:raise ValueError('Different draw fixture')
    for name,sha in manifest['outputs'].items():
        if digest(draw/name)!=sha:raise ValueError('Draw provenance mismatch')
    target=args.output/'draw.bin'
    execute([str(args.native),'effect34-geometry',str(args.efd),str(args.textures),str(draw/'cases.bin'),str(target)],'draw')
    maximum=compare_draws((draw/'original.bin').read_bytes(),target.read_bytes(),len(steps))
    vertices=compare_vertices((draw/'original.bin.packets.bin').read_bytes(),Path(str(target)+'.vertices.bin').read_bytes(),Path(str(target)+'.materials.bin').read_bytes(),fixture)
    report={'scope':__doc__,'state_words_exact':452*645,'quads':quad_result,'source_sharing':share,'source_storage':storage_scope,'draw_cases':len(steps),'draw_source_records':steps,'vertices_exact':vertices,'matrix_max_absolute_error':maximum,'matrix_xyz_tolerance':0,'colors_matrix_w_exact':True,'original_update_run':run,'original_draw_run':manifest,'original_state_sha256':STATE_SHA,'original_quads_sha256':QUADS_SHA,'original_storage_sha256':STORAGE_SHA,'native_outputs_sha256':{x.name:digest(x)for x in args.output.iterdir()if x.is_file()},'native_sha256':digest(args.native),'efd_sha256':digest(args.efd),'textures_sha256':{x.name:digest(x)for x in args.textures.glob('*.xim')},'commands':commands}
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print('Effect34 state/quads and original draw fixtures compared')

if __name__=='__main__':main()
