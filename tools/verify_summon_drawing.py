#!/usr/bin/env python3
"""Compare summoning source geometry/materials and bounded deferred transforms.

Research RAM is used only by the original helper oracle, never native code.
Synthetic transforms do not prove scene RNG, raster equality or progression.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from verify_event import write_exact
from verify_map_effect import digest, ELF_SHA

METADATA = {
    33: ((40, 0, 1, 0xffffffff), (25, 0x90, 1, 0xffffffff),
         (59, 0, 1, 25), *((29, 0x101, 1, 5),) * 6),
    115: ((32, 5, 0x40, 0xffffffff), *((31, 0x4005, 0x40, 17),) * 4),
}

def fixture(template):
    data = bytearray()
    identity = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
    for case in range(5):
        job = ((0, 0, 0, 1), (0, 0, 0, 0), (1, 1, 1, 1), (1, 1, 1, 1)) if not case else (
            (3*case, -4*case, 5*case, 1), (.031*case, -.077*case, .019*case, 0),
            (.5+case/4, 1.5, 2, 1), (.4, .6, .8, .5))
        camera = (0, 0, 0, 1) if not case else (.123*case, -.231*case, .02*case, 1)
        view = identity if case < 3 else (0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0, 7, -3, 11, 1)
        data += struct.pack('<I37f', case, *(x for v in job for x in v), *camera, *view, 450)
        for i, metadata in enumerate(METADATA[template]):
            channels = ((i*.2, -i*.4, 5+i*.1, 0), (.025*i, .03*i, -.07*i, 0),
                        (2.5, 3.5, 1, 1), (.65, .55, .45, .75))
            data += struct.pack('<16f4I', *(x for v in channels for x in v), *metadata)
        if template == 33:
            data += struct.pack('<30I', *((-192+3*i) & 0xffff for i in range(30)))
    return bytes(data)

def unpack_records(packet, wanted, fmt):
    """Decode the bounded retail VIF packet, respecting UNPACK chunk padding."""
    if len(packet) < 32 or len(packet) % 16:
        raise ValueError('Summoning packet extent')
    word = lambda at: struct.unpack_from('<I', packet, at)[0]
    if (word(0) & 0xffff)*16+32 != len(packet):
        raise ValueError('Summoning DMA extent')
    at, records = 16, []
    strides = {0x60: 4, 0x68: 12, 0x6c: 16, 0x6e: 4, 0x75: 4, 0x79: 6}
    while at < len(packet)-16:
        command = word(at); at += 4
        op, count = command >> 24, (command >> 16) & 255
        if op in (0x79, 0x75) and command & 0xffff != {0x79: 0x8001, 0x75: 0x41aa}[op]:
            raise ValueError('Summoning VIF vertex destination')
        if op in strides:
            size = (count or 256)*strides[op]
        elif op == 0x20:
            size = 16
        elif op in (0, 1, 4, 0x14, 0x17):
            size = 0
        else:
            raise ValueError(f'Unreviewed summoning VIF command {command:08x}')
        padded = (size+3)//4*4
        if at+padded > len(packet)-16:
            raise ValueError('Summoning VIF payload extent')
        if op == wanted:
            records.append(list(struct.iter_unpack(fmt, packet[at:at+size])))
        at += padded
    return records

def packet_vertices(data, template):
    at, result = 0, []
    for element, (shape, _, _, texture) in enumerate(METADATA[template]):
        if len(data)-at < 44:
            raise ValueError('Summoning packet header extent')
        index, got_shape, uvmin, uvmax, *tail = struct.unpack_from('<11I', data, at); at += 44
        material, pieces = tail[:6], tail[6]
        expected_uv = {25: 0x1f007f, 5: 0x17001f, 17: 0x1f001f, 0xffffffff: 0xffffffff}[texture]
        expected_material = [0, 1, 0x48, 0, 0x50000, 0] if template == 33 else [0, 1, 0x44, 0, 0x3000d, 0]
        if (index, got_shape, uvmin, uvmax, pieces) != (element, shape, 0, expected_uv, 2 if shape == 59 else 1):
            raise ValueError('Summoning source identity/texture/pieces')
        if material != expected_material:
            raise ValueError('Summoning source material')
        triangles = []
        for piece in range(pieces):
            packets = []
            for kind in range(4):
                if len(data)-at < 4:
                    raise ValueError('Summoning packet size missing')
                size = struct.unpack_from('<I', data, at)[0]; at += 4
                if not size and kind == 1 and texture == 0xffffffff:
                    packets.append(b''); continue
                if size < 64 or size > 8192 or size % 16 or size > len(data)-at:
                    raise ValueError('Summoning packet extent')
                packets.append(data[at:at+size]); at += size
            pos, uv, color, indices = packets
            positions = [v for chunk in unpack_records(pos, 0x79, '<3h') for v in chunk]
            coords = [v for chunk in unpack_records(uv, 0x75, '<2H') for v in chunk] if uv else [(0, 0)]
            colors = [v for chunk in unpack_records(color, 0x6e, '<4B') for v in chunk]
            expected_positions = {40: 17, 25: 60, 59: (95, 20)[piece], 29: 4, 31: 4, 32: 4}[shape]
            if len(positions) != expected_positions or len(colors) != (2 if shape in (40, 25) else 1):
                raise ValueError('Summoning source vertex/color count')
            for strip in unpack_records(indices, 0x6e, '<4B'):
                if len(strip) < 3:
                    raise ValueError('Summoning empty triangle strip')
                corners = []
                for p, normal, c, t in strip:
                    if normal or p >= len(positions) or c >= len(colors) or t >= len(coords):
                        raise ValueError('Summoning corner index')
                    corners.append((*[v/64 for v in positions[p]], 1., *colors[c],
                                    coords[t][0]/32768, coords[t][1]/32768, 0., 0.))
                for i in range(len(corners)-2):
                    triangles.extend(corners[i:i+3])
        expected_corners = {40: 48, 25: 180, 59: 300, 29: 6, 31: 6, 32: 6}[shape]
        if len(triangles) != expected_corners:
            raise ValueError('Summoning triangle count')
        result.append(triangles)
    if at != len(data):
        raise ValueError('Trailing summoning packets')
    return result

def compare_vertices(packets, native, template):
    at, total = 0, 0
    for i, expected in enumerate(packet_vertices(packets, template)):
        if len(native)-at < 12:
            raise ValueError('Native summoning geometry header extent')
        element, count, texture = struct.unpack_from('<3I', native, at); at += 12
        if (element, count, texture) != (i, len(expected), METADATA[template][i][3]):
            raise ValueError('Native summoning geometry identity/count/texture')
        size = count*48
        if len(native)-at < size:
            raise ValueError('Native summoning vertex extent')
        actual = list(struct.iter_unpack('<12f', native[at:at+size])); at += size
        for v, (ref, got) in enumerate(zip(expected, actual)):
            if ref != got:
                raise ValueError(f'Summoning{template} element{i} vertex{v}: {ref} != {got}')
        total += count
    if at != len(native):
        raise ValueError('Trailing native summoning vertices')
    return total

def compare_matrices(original, native, template):
    stride = 1+20*len(METADATA[template])
    if len(original) != 5*stride*4 or len(native) != len(original):
        raise ValueError('Summoning transform extent')
    # No tolerance: both sides execute the recovered float rounding/sine path.
    for case, (ref, got) in enumerate(zip(struct.iter_unpack(f'<{stride}I', original),
                                         struct.iter_unpack(f'<{stride}I', native))):
        if ref[0] != case or got[0] != case:
            raise ValueError('Summoning transform case identity')
        for word, (a, b) in enumerate(zip(ref[1:], got[1:])):
            if a != b:
                values = [struct.unpack('<f', struct.pack('<I', v))[0] for v in (a, b)]
                raise ValueError(f'Summoning{template} case{case} word{word}: {values}')
    return 5*len(METADATA[template])

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native-release/okage-assets'))
    parser.add_argument('--reference', type=Path, default=Path('local/summon-drawing-reference-v2'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    if digest(Path('local/extracted/SCUS_971.29')) != ELF_SHA:
        raise ValueError('Different retail ELF')
    efd = Path('local/effect-input/CMNDATA/2D/TES.EFD')
    textures = Path('local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF')
    report = {'scope': __doc__, 'elf_sha256': ELF_SHA, 'efd_sha256': digest(efd),
              'native_sha256': digest(args.native), 'cases': []}
    for template in METADATA:
        inp = args.reference/f'{template}.input.bin'
        original = args.reference/f'{template}.original.bin'
        packets = Path(str(original)+'.packets.bin')
        run = json.loads((args.reference/f'{template}.run.json').read_text())
        if run['exit_code'] or run['elf_sha256'] != ELF_SHA or inp.read_bytes() != fixture(template):
            raise ValueError('Incomplete or changed original draw probe')
        for key, path in [('input_sha256', inp), ('output_sha256', original), ('packets_sha256', packets)]:
            if digest(path) != run[key]:
                raise ValueError('Original draw hash mismatch')
        packet_vertices(packets.read_bytes(), template)
        dst = args.output/f'{template}.native.bin'
        command = [str(args.native), f'summon{template}-effect-geometry', str(efd), str(textures), str(inp), str(dst)]
        result = subprocess.run(command, capture_output=True)
        write_exact(args.output/f'{template}.log', result.stdout+result.stderr)
        write_exact(args.output/f'{template}.run.json', (json.dumps({'command': command,
                    'exit_code': result.returncode, 'binary_sha256': digest(args.native)}, indent=2)+'\n').encode())
        result.check_returncode()
        transforms = compare_matrices(original.read_bytes(), dst.read_bytes(), template)
        vertices = compare_vertices(packets.read_bytes(), Path(str(dst)+'.vertices.bin').read_bytes(), template)
        materials = Path(str(dst)+'.materials.bin').read_bytes()
        expected = b''.join(struct.pack('<3I', i, template == 33, template == 33) for i in range(len(METADATA[template])))
        if materials != expected:
            raise ValueError('Native summoning blend/depth material differs')
        report['cases'].append({'template': template, 'transforms_colors_exact': transforms,
                               'source_vertices_exact': vertices, 'original_run': run,
                               'native_command': command, 'native_sha256': digest(dst),
                               'vertices_sha256': digest(Path(str(dst)+'.vertices.bin'))})
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(json.dumps({'templates': list(METADATA), 'all_transforms_colors_vertices_materials_exact': True}))

if __name__ == '__main__':
    main()
