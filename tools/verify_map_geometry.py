#!/usr/bin/env python3
"""Compare map-effect source geometry and bounded original transform execution.

Checks template25's captured packet topology, quantized XYZ/UV/colors, GS state,
and five synthetic job/camera cases. Does not validate raster pixels, live scene
ordering, effect updates or physical presentation. RAM is research input only.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def compare_matrices(original, native):
    if len(original) != 5*161*4 or len(native) != len(original):
        raise ValueError('Geometry transform interval extent')
    maximum = 0.
    for case, (ref, got) in enumerate(zip(struct.iter_unpack('<161I', original),
                                         struct.iter_unpack('<161I', native))):
        if ref[0] != case or got[0] != case:
            raise ValueError('Geometry case identity')
        for at, (a, b) in enumerate(zip(ref[1:], got[1:])):
            if a == b:
                continue
            channel = at % 20
            if channel >= 16:
                raise ValueError('Effect color/fade differs')
            x, y = (struct.unpack('<f', struct.pack('<I', v))[0] for v in (a, b))
            error = abs(x-y)
            if not math.isfinite(error) or error > (0 if channel % 4 == 3 else 2e-5):
                raise ValueError(f'Effect matrix differs: case{case} word{at}')
            maximum = max(maximum, error)
    return maximum

def packet_vertices(data, template=25):
    if template not in (17,19,25,26):raise ValueError("Unreviewed geometry template")
    at = 0
    result = []
    for element in range(9 if template in (17,19) else 2 if template==26 else 8):
        if len(data)-at < 40:
            raise ValueError('Geometry packet header extent')
        index, shape, uvmin, uvmax, zlo, zhi, alo, ahi, tlo, thi = struct.unpack_from('<10I', data, at)
        at += 40
        if (index, shape) != (element, 31 if template in (17,19) else 24 if element < 3 else 29):
            raise ValueError('Geometry packet identity')
        alpha_blend=template in (17,19) or (template==26 and element==0)
        if (zlo, zhi, alo, ahi, tlo, thi) != (0, 1, 0x44 if alpha_blend else 0x48, 0, 0x5000d if alpha_blend else 0x50000, 0) or uvmin:
            raise ValueError('Unreviewed effect material')
        if template==26 and uvmax!=0x003f003f:
            raise ValueError('Cylinder texture region')
        packets = []
        for _ in range(4):
            if len(data)-at < 4:
                raise ValueError('Missing geometry packet extent')
            size, = struct.unpack_from('<I', data, at);at += 4
            if size < 64 or size > 8192 or size % 16 or size > len(data)-at:
                raise ValueError('Invalid geometry packet extent')
            packets.append(data[at:at+size]);at += size
        pos, uv, color, indices = packets
        word = lambda p, offset: struct.unpack_from('<I', p, offset)[0]
        counts = (word(pos, 28), word(uv, 28), word(color, 20), (word(indices, 44) >> 16) & 255)
        expected = (32, 34, 2, 34) if shape == 24 else (4, 4, 1, 4)
        if counts != expected:
            raise ValueError('Unexpected source geometry counts')
        if (word(pos,44), word(uv,44), word(color,36), word(indices,44)) != (
                0x79008001 | counts[0] << 16, 0x750041aa | counts[1] << 16,
                0x6e004149 | counts[2] << 16, 0x6e00c001 | counts[3] << 16):
            raise ValueError('Unexpected geometry UNPACK format')
        positions = list(struct.iter_unpack('<3h', pos[48:48+counts[0]*6]))
        coords = list(struct.iter_unpack('<2H', uv[48:48+counts[1]*4]))
        colors = list(struct.iter_unpack('<4B', color[40:40+counts[2]*4]))
        corners = list(struct.iter_unpack('<4B', indices[48:48+counts[3]*4]))
        triangles = []
        for triangle in range(len(corners)-2):
            for corner in corners[triangle:triangle+3]:
                p, normal, c, t = corner
                if normal or p >= len(positions) or c >= len(colors) or t >= len(coords):
                    raise ValueError('Geometry corner index')
                triangles.append((*[x/64 for x in positions[p]], 1., *colors[c],
                                  coords[t][0]/32768, coords[t][1]/32768, 0., 0.))
        result.append(triangles)
    if at != len(data):
        raise ValueError('Trailing original geometry packets')
    return result

def compare_vertices(packets, native, template=25):
    expected = packet_vertices(packets, template)
    at = 0
    total = 0
    for i, triangles in enumerate(expected):
        if len(native)-at < 12:
            raise ValueError('Native geometry header extent')
        element, count, texture = struct.unpack_from('<3I', native, at);at += 12
        if (element, count, texture) != (i, len(triangles), 17 if template in (17,19) else 20 if i < 3 else 13):
            raise ValueError('Native geometry identity')
        if len(native)-at < count*48:
            raise ValueError('Native geometry vertex extent')
        actual = list(struct.iter_unpack('<12f', native[at:at+count*48]));at += count*48
        if actual != triangles:
            raise ValueError('Native XYZ/UV/color/topology differs')
        total += count
    if at != len(native):
        raise ValueError('Trailing native geometry')
    return total

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--reference', type=Path, default=Path('local/map-geometry-v1'))
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args();args.output.mkdir(parents=True, exist_ok=False)
    source = args.reference/'cases.bin'
    original = args.reference/'original.bin'
    packets = Path(str(original)+'.packets.bin')
    manifest = json.loads((args.reference/'original.run.json').read_text())
    if manifest['exit_code'] != 0 or any(digest(path) != manifest[key] for path,key in [
            (source,'input_sha256'), (original,'output_sha256'), (packets,'packets_sha256')]):
        raise ValueError('Original geometry provenance mismatch')
    output = args.output/'native.bin'
    command = [str(args.native), 'map-effect-geometry', 'local/effect-input/CMNDATA/2D/TES.EFD',
               'local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF', str(source), str(output)]
    subprocess.run(command, check=True)
    maximum = compare_matrices(original.read_bytes(), output.read_bytes())
    vertices = compare_vertices(packets.read_bytes(), Path(str(output)+'.vertices.bin').read_bytes())
    report = {'scope': __doc__.strip(), 'cases': 5, 'element_transforms': 40, 'matrix_tolerance': 2e-5,
        'matrix_max_absolute_error': maximum, 'color_and_fade_exact': True,
        'source_vertices_exact': vertices, 'material': 'ALPHA0x48, ZBUF mask1, TEST0x50000, TEX1=0x60',
        'native_command': command, 'native_binary_sha256': digest(args.native),
        'original_run': manifest, 'efd_sha256': digest(Path(command[2]))}
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(f'{vertices} source vertices exact;40 transforms within{maximum:g};color/fade exact')

if __name__ == '__main__':
    main()
