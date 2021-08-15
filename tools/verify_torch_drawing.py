#!/usr/bin/env python3
"""Compare torch91 source geometry/materials and bounded original draw transforms.

Five synthetic job/view/camera cases. No raster, scene scheduling, interior
loading or whole-game fidelity claim. Original RAM stays research-only.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_map_effect import digest, ELF_SHA

ORIGINAL_SHA = 'e9a3af6bef926a3bd29d019a734233db6a0a359564c0a43e4f6e164f43456d9d'
PACKETS_SHA = '54213a8540e0e94be8c018b7fe76fd9fd446756c84d89f1c1dc9dea4b92603d7'

def fixture(deferred=False):
    data = bytearray()
    identity = (1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)
    for case in range(5):
        job = ((0,0,0,1), (0,0,0,0), (1,1,1,1), (1,1,1,1)) if not case else (
            (3*case,-4*case,5*case,1), (.031*case,-.077*case,.019*case,0),
            (.5+case/4,1.5,2,1), (.4,.6,.8,.5))
        camera = (0,0,0,1) if not case else (.123*case,-.231*case,.02*case,1)
        view = identity if case < 3 else (0,0,1,0, 0,1,0,0, -1,0,0,0, 7,-3,11,1)
        data += struct.pack('<I37f', case, *(x for v in job for x in v), *camera, *view, 450)
        for i in range(3 if deferred else 4):
            channels = ((i*.2,-i*.4,i*.1,0), (.025*i,.03*i,-.07*i,0),
                        (2.5,3.5,1,1), (.65,.55,.45,.75))
            data += struct.pack('<16f4I', *(x for v in channels for x in v),
                                28 if i < 3 else 40, 0x108 if deferred else 0x158 if i < 3 else 2,
                                8 if deferred else 1, 4 if i < 3 else 0xffffffff)
        for i in range(3):
            sign = -1 if i == 1 else 1
            for x,y,u,v in ((32,-64,32,0),(-32,-64,0,0),(32,0,32,65),(-32,0,0,65)):
                data += struct.pack('<3hH2H', x*sign,y,0,0,(u+i*32)*32,v*32)
    return bytes(data)

def compare_matrices(original, native, deferred=False):
    stride = 61 if deferred else 81
    if len(original) != 5*stride*4 or len(native) != len(original):
        raise ValueError('Torch draw transform extent')
    maximum = 0.
    for case, (ref, got) in enumerate(zip(struct.iter_unpack(f'<{stride}I', original),
                                         struct.iter_unpack(f'<{stride}I', native))):
        if ref[0] != case or got[0] != case:
            raise ValueError('Torch draw case identity')
        for at,(a,b) in enumerate(zip(ref[1:], got[1:])):
            if a == b:
                continue
            channel = at % 20
            if channel >= 16 or channel % 4 == 3:
                raise ValueError('Torch color or homogeneous matrix differs')
            x,y = (struct.unpack('<f',struct.pack('<I',v))[0] for v in (a,b))
            error = abs(x-y)
            # Same tolerance as the earlier original VU/native sine matrices.
            if not math.isfinite(error) or error > 2e-5:
                raise ValueError(f'Torch matrix differs: case{case} word{at}: {x} != {y}')
            maximum = max(maximum,error)
    return maximum

def packet_vertices(data, deferred=False):
    at, result = 0, []
    word = lambda p,a: struct.unpack_from('<I',p,a)[0]
    for element in range(3 if deferred else 4):
        if len(data)-at < 40:
            raise ValueError('Torch packet header extent')
        index,shape,uvmin,uvmax,*material = struct.unpack_from('<10I',data,at);at += 40
        if (index,shape) != (element,28 if element < 3 else 40):
            raise ValueError('Torch source identity')
        if material != [0,1,0x48,0,0x53001 if deferred else 0x50000,0] or uvmin or uvmax!=(0x3f005f if element<3 else 0xffffffff):
            raise ValueError('Torch source material')
        packets = []
        for packet in range(4):
            if len(data)-at < 4:
                raise ValueError('Torch packet size missing')
            size = word(data,at);at += 4
            if size == 0 and element == 3 and packet == 1:
                packets.append(b'');continue
            if size < 64 or size > 8192 or size % 16 or size > len(data)-at:
                raise ValueError('Torch packet extent')
            packets.append(data[at:at+size]);at += size
        pos,uv,color,indices = packets
        n = 4 if element < 3 else 17
        nc = 1 if element < 3 else 2
        if (word(pos,28),word(pos,44),word(color,20),word(color,36)) != (
                n,0x79008001|(n<<16),nc,0x6e004149|(nc<<16)):
            raise ValueError('Torch source packing')
        positions = list(struct.iter_unpack('<3h',pos[48:48+n*6]))
        colors = list(struct.iter_unpack('<4B',color[40:40+nc*4]))
        if element < 3:
            if word(uv,28)!=4 or word(uv,44)!=0x750441aa or word(indices,44)!=0x6e04c001:
                raise ValueError('Torch flame topology/UV format')
            coords = list(struct.iter_unpack('<2H',uv[48:64]))
            strip = list(struct.iter_unpack('<4B',indices[48:64]))
            corners = strip[:3]+strip[1:]
        else:
            coords,corners = [(0,0)],[]
            for offset in list(range(44,44+13*32,32))+[468,500,532]:
                group = len(corners)//3
                destination = 1+4*(group if group<13 else group-13)
                if word(indices,offset)!=0x6e030000|0xc000|destination:
                    raise ValueError('Torch glow triangle UNPACK')
                corners.extend(struct.iter_unpack('<4B',indices[offset+4:offset+16]))
        triangles = []
        for p,normal,c,t in corners:
            if normal or p>=len(positions) or c>=len(colors) or t>=len(coords):
                raise ValueError('Torch corner index')
            triangles.append((*[v/64 for v in positions[p]],1.,*colors[c],
                              coords[t][0]/32768,coords[t][1]/32768,0.,0.))
        result.append(triangles)
    if at!=len(data):
        raise ValueError('Trailing torch packets')
    return result

def compare_vertices(packets,native,deferred=False):
    at,total = 0,0
    for i,expected in enumerate(packet_vertices(packets,deferred)):
        if len(native)-at < 12:
            raise ValueError('Native torch geometry header extent')
        element,count,texture = struct.unpack_from('<3I',native,at);at += 12
        if (element,count,texture)!=(i,len(expected),4 if i<3 else 0xffffffff):
            raise ValueError('Native torch geometry identity')
        if len(native)-at<count*48:
            raise ValueError('Native torch geometry extent')
        actual = list(struct.iter_unpack('<12f',native[at:at+count*48]));at += count*48
        if actual != expected:
            raise ValueError(f'Torch XYZ/UV/color/topology differs: element{i}')
        total += count
    if at != len(native):
        raise ValueError('Trailing native torch geometry')
    return total

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,default=Path('build/native-release/okage-assets'))
    p.add_argument('--reference',type=Path,default=Path('local/torch-drawing-reference-v2'))
    p.add_argument('--output',type=Path)
    p.add_argument('--prepare',type=Path)
    args=p.parse_args()
    if args.prepare:
        args.prepare.mkdir(parents=True,exist_ok=False)
        write_exact(args.prepare/'cases.bin',fixture());return
    if not args.output:p.error('--output is required')
    source=args.reference/'cases.bin';original=args.reference/'original.bin'
    packets=Path(str(original)+'.packets.bin')
    run=json.loads((args.reference/'original.run.json').read_text())
    if run['exit_code'] or digest(Path('local/extracted/SCUS_971.29'))!=ELF_SHA:
        raise ValueError('Incomplete original probe or different ELF')
    if digest(original)!=ORIGINAL_SHA or digest(packets)!=PACKETS_SHA:
        raise ValueError('Unreviewed original torch drawing output')
    if source.read_bytes()!=fixture() or any(digest(path)!=run[key] for path,key in (
            (source,'input_sha256'),(original,'output_sha256'),(packets,'packets_sha256'))):
        raise ValueError('Torch draw provenance differs')
    args.output.mkdir(parents=True,exist_ok=False);output=args.output/'native.bin'
    command=[str(args.native),'torch-effect-geometry','local/effect-input/CMNDATA/2D/TES.EFD',
             'local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF',str(source),str(output)]
    result=subprocess.run(command,capture_output=True)
    write_exact(args.output/'native.log',result.stdout+result.stderr)
    write_exact(args.output/'native.run.json',(json.dumps({'command':command,'exit_code':result.returncode,
                'binary_sha256':digest(args.native)},indent=2)+'\n').encode())
    result.check_returncode()
    maximum=compare_matrices(original.read_bytes(),output.read_bytes())
    count=compare_vertices(packets.read_bytes(),Path(str(output)+'.vertices.bin').read_bytes())
    report={'scope':__doc__,'cases':5,'element_transforms':20,'matrix_tolerance':2e-5,
            'matrix_max_absolute_error':maximum,'colors_exact':True,'source_vertices_exact':count,
            'original_run':run,'native_command':command,'native_binary_sha256':digest(args.native),
            'efd_sha256':digest(Path(command[2]))}
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(f'{count} torch vertices exact;20 transforms within {maximum:g};colors exact')

if __name__=='__main__':main()
