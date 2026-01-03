#!/usr/bin/env python3
"""Verify USA font metrics, original atlas inputs and native dialogue layout.

Research snapshots/oracles are validation inputs only. Native commands consume
disc fonts, PME data and (for the timing replay only) the documented trace seed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib
from inspect_disc import Disc
from verify_event import write_exact
from verify_text_frames import frame_outputs

def digest(data):
    return hashlib.sha256(data).hexdigest()

def pam_png(source, dest):
    data = source.read_bytes()
    header, pixels = data.split(b'ENDHDR\n', 1)
    fields = dict(line.split(b' ', 1) for line in header.splitlines()[1:])
    width, height = int(fields[b'WIDTH']), int(fields[b'HEIGHT'])
    if fields[b'DEPTH'] != b'4' or len(pixels) != width*height*4:
        raise ValueError('Unexpected PAM')
    def chunk(kind, value):
        return struct.pack('>I', len(value))+kind+value+struct.pack('>I', zlib.crc32(kind+value))
    scanlines = b''.join(b'\0'+pixels[y*width*4:(y+1)*width*4] for y in range(height))
    png = b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>2I5B', width, height, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(scanlines))+chunk(b'IEND', b'')
    write_exact(dest, png)
    return pixels

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--output', type=Path, default=Path('local/font-validation'))
    args = p.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    native = [str(args.native)]
    oracle = ['build/play-research/okage-oracle/okage-pose-oracle']
    elf = Path('local/extracted/SCUS_971.29')
    ram = Path('local/research/opening600-ram.bin').read_bytes()
    if len(ram) != 64*1024*1024:
        raise ValueError('Expected opening RAM extent')
    ee = out/'opening600-ee.bin'
    write_exact(ee, ram[:32*1024*1024])
    word = lambda at: struct.unpack_from('<I', ram, at)[0]
    table = word(0x2003ec)
    report = {'scope': 'USA FNT import, glyph metrics, unit-scale normal-dialogue layout; flat rendering is a diagnostic, not faithful window/GS presentation',
              'native_sha256': digest(args.native.read_bytes()), 'elf_sha256': digest(elf.read_bytes()),
              'ram_sha256': digest(ram), 'fonts': []}
    palette = struct.unpack_from('<16I', ram, 0x2003f8)
    if palette != (0,)+tuple(0xffffff+((136-i*8)<<24) for i in range(1, 16)):
        raise ValueError('Unexpected original font palette')
    disc = Disc('local/disc/Okage - Shadow King (USA).bin')
    font_dir = Path('local/font-input/CMNDATA/FONT')
    metrics = []
    try:
        inventory = {f['path']: f for f in disc.inventory()}
        for handle, name in enumerate(('IQ24.FNT', 'BMUI.FNT', 'BM.FNT')):
            entry = inventory['CMNDATA/FONT/'+name]
            data = disc.read(entry['lba'], entry['size'])
            source = font_dir/name
            source.parent.mkdir(parents=True, exist_ok=True)
            write_exact(source, data)
            for label, command in (('native', native+['font-metrics']), ('retail', oracle+['font-metrics', str(elf)])):
                subprocess.run(command+[str(source), str(out/(name+'.'+label))], check=True)
            converted = (out/(name+'.native')).read_bytes()
            count = data[5]-data[4]+1
            address = word(table+handle*60+16)
            if converted != (out/(name+'.retail')).read_bytes() or converted != ram[address:address+count*20]:
                raise ValueError('Original font metric mismatch: '+name)
            image_at = 8+count*20
            height, width = struct.unpack_from('<HH', data, image_at)
            packed = data[image_at+4:]
            address = word(table+handle*60+32)
            if packed != ram[address:address+len(packed)]:
                raise ValueError('Original packed atlas mismatch: '+name)
            pam = out/(name+'.pam')
            subprocess.run(native+['font', str(source), str(pam)], check=True)
            rgba = pam_png(pam, out/(name+'.png'))
            expected = bytearray()
            for byte in packed:
                for index in (byte & 15, byte >> 4):
                    color = palette[index]
                    expected.extend((color & 255, color >> 8 & 255, color >> 16 & 255, min(255, (color >> 24)*2)))
            if rgba != expected:
                raise ValueError('Font RGBA expansion mismatch: '+name)
            metrics.append((data[4], list(struct.iter_unpack('<10H', converted))))
            report['fonts'].append({'name': name, 'input_sha256': digest(data), 'glyphs': count,
                                    'metrics_exact_original_loader_and_capture': True, 'metrics_sha256': digest(converted),
                                    'width': width, 'height': height, 'packed_pixels_exact_capture': True, 'rgba_sha256': digest(rgba)})
    finally:
        disc.close()
    fixtures = []
    for font, (first, glyphs) in enumerate(metrics):
        for code in range(32, 128):
            if code == 92:
                continue
            m = glyphs[code-first]
            for x, y, color in ((0, 0, 0x80808080), (157, 24, 0xe4), (-10, -20, 0xff7f0183)):
                fixtures.append((code, font, color, x & 0xffffffff, y & 0xffffffff,
                                 10 if code == 32 else m[8]//16, 24 if code == 32 else m[9]//16))
    fixture = out/'glyph-input.bin'
    write_exact(fixture, b''.join(struct.pack('<7I', *r) for r in fixtures))
    subprocess.run(native+['text-glyphs', str(font_dir), str(fixture), str(out/'glyphs.native')], check=True)
    subprocess.run(oracle+['text-glyphs', str(elf), str(ee), str(fixture), str(out/'glyphs.retail')], check=True)
    if (out/'glyphs.native').read_bytes() != (out/'glyphs.retail').read_bytes():
        raise ValueError('Original glyph placement mismatch')
    report['glyph_layout'] = {'cases': len(fixtures), 'exact': True, 'output_sha256': digest((out/'glyphs.native').read_bytes()),
                             'scope': 'Original 0x19c568, all supported printable single-byte glyphs in three fonts; unit scale, zero offsets, normal left bearing'}
    trace = Path('local/reference-text-760-v2.bin')
    rows = list(struct.iter_unpack('<40I', trace.read_bytes()))
    expected_xy = {}
    for r in rows:
        if r[1] in (0x198390, 0x1984f4):
            expected_xy[r[0]] = (r[29], r[30])
    output = out/'layout.bin'
    subprocess.run(native+['text-layout-replay', 'local/text-input/FLDDATA/MES/FIELD.PME', '10100', str(trace), str(font_dir), str(output)], check=True)
    expected = [r+expected_xy[i] for i, r in enumerate(frame_outputs(rows))]
    actual = list(struct.iter_unpack('<13I', output.read_bytes()))
    if actual != expected:
        raise ValueError('Original layout/cursor/clock mismatch')
    pam_png(Path(str(output)+'.pam'), out/'dialogue.png')
    report['continuous_replay'] = {'frames': len(actual), 'exact_words_per_frame': 13, 'output_sha256': digest(output.read_bytes()),
                                   'scope': 'Prior cursor/clock replay plus native x/y advancement; one captured initial state and observed public input calls'}
    report['rendering_limit'] = 'Atlas and final-page PNGs are native diagnostic outputs. Window geometry, placement, fades, filtering, clipping, shadows and full original pixels are not validated.'
    (out/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
