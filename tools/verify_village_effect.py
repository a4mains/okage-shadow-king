#!/usr/bin/env python3
"""Audit template19 smoke construction, 700 updates and source geometry.

Uses a completed isolated original probe; does not launch a VM or establish
whole-scene scheduling/RNG, raster pixels, startup completion or playability.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from verify_event import digest, write_exact
from verify_map_effect import compare, ELF_SHA
from verify_map_geometry import compare_vertices

STATE_SHA = '8369ef2481f5838b665082df3373641cbcb9c8b434772209591e59b6a1208904'
PACKETS_SHA = '2edcdf8fdf077b070671d61544769731225b51974bfe35953722534dccd848d3'
RAM_SHA = 'cd1420a81e5e838de19bca6e454cdae90880faa6cc3a7e7b0e13d4cd518279ee'

def fixture():
    # First village chimney transform; reserved selector0xffc leaves parameters
    # intact while exercising the actual field-effect setter's no-write branch.
    return struct.pack('<4I16f', 19, 1, 0xffc, 0x100, -15,-32,36.5,1,
                       0,1,1,0, 1,1,1,0, 0,0,0,0)

def original_sine_check(reference, ram):
    """Research-only diagnosis: original table explains native libm differences."""
    def f32(x):
        v = struct.unpack('<f', struct.pack('<f', x))[0]
        if abs(v) > abs(x):
            bits = struct.unpack('<I', struct.pack('<f', v))[0]
            v = struct.unpack('<f', struct.pack('<I', bits-1))[0]
        return v
    if len(ram) != 32*1024*1024 or len(reference) != 702*425*4:
        raise ValueError('Original sine diagnosis fixture extent')
    checked = 0
    for step, row in enumerate(struct.iter_unpack('<425I', reference)):
        if step < 2:
            continue
        def number(i):
            return struct.unpack('<f', struct.pack('<I', row[i]))[0]
        for element in range(9):
            at = 29+element*44
            t = number(at+18)
            cycle = f32(t*f32(float(row[5])*f32(.01)))
            turn = f32(cycle-int(cycle))
            angle = (int(f32(turn*32768)) >> 1) & 16383
            sine = struct.unpack_from('<f', ram, 0x202ee0+angle*4)[0]
            for axis in (0, 1):
                expected = f32(f32(f32(sine*number(27))+number(at+8+axis))+f32(t+t))
                if expected != number(at+32+axis):
                    raise ValueError('Original-table smoke scale diagnosis differs')
                checked += 1
    return checked

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--reference', type=Path, default=Path('local/village-effect-probe-v1'))
    p.add_argument('--output', type=Path)
    p.add_argument('--prepare', type=Path)
    args = p.parse_args()
    if args.prepare:
        args.prepare.mkdir(parents=True, exist_ok=False)
        write_exact(args.prepare/'19.input.bin', fixture())
        return
    if not args.output:
        p.error('--output is required for an audit')
    source, reference, packets = (args.reference/name for name in
                                ('19.input.bin', '19.original.bin', '19.original.bin.packets.bin'))
    run = json.loads((args.reference/'original.run.json').read_text())
    ram = Path('local/research/village300-v1-ee.bin')
    if run['exit_code'] or digest(Path('local/extracted/SCUS_971.29')) != ELF_SHA:
        raise ValueError('Incomplete original probe or different ELF')
    if source.read_bytes() != fixture() or digest(source) != run['input_sha256']:
        raise ValueError('Different reviewed smoke request')
    if digest(reference) != STATE_SHA or run['output_sha256'] != STATE_SHA:
        raise ValueError('Different reviewed smoke state')
    if digest(packets) != PACKETS_SHA or run['packets_sha256'] != PACKETS_SHA:
        raise ValueError('Different reviewed smoke source geometry/material')
    if digest(ram) != RAM_SHA or run['ram_sha256'] != RAM_SHA:
        raise ValueError('Different original fixture RAM')
    args.output.mkdir(parents=True, exist_ok=False)
    target = args.output/'native.bin'
    efd = Path('local/effect-input/CMNDATA/2D/TES.EFD')
    textures = Path('local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF')
    command = [str(args.native), 'map-effect-probe', str(efd), str(textures), str(source), '700', str(target)]
    subprocess.run(command, check=True)
    report = compare(reference.read_bytes(), target.read_bytes(), 19, 9, 700)
    report.update(scope=__doc__, original_run=run, command=command, elf_sha256=ELF_SHA,
                  native_binary_sha256=digest(args.native), native_state_sha256=digest(target),
                  efd_sha256=digest(efd), scale_tolerance=2e-6,
                  exact_vertices=compare_vertices(packets.read_bytes(), Path(str(target)+'.vertices.bin').read_bytes(), 19),
                  original_table_exact_scale_words=original_sine_check(reference.read_bytes(), ram.read_bytes()),
                  material='ALPHA0x44, ZBUF mask1, TEST0x5000d; shape31 texture17')
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print('Smoke constructor/700 updates compared;54 source vertices exact')

if __name__ == '__main__':
    main()
