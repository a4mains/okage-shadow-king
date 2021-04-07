#!/usr/bin/env python3
"""Audit dust17 constructor/31 updates, source geometry and effect-light tint.

Uses completed original probes; never launches a VM. Does not establish whole
scene RNG/order, raster pixels, physical controls or general effect support.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_map_geometry import compare_vertices

STATE_SHA = '16ff0b25c13acdd5b964f4154392ec1b16cd1795e90e39466f1bd7f086da677c'
ELF_SHA = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
PACKETS_SHA = '2edcdf8fdf077b070671d61544769731225b51974bfe35953722534dccd848d3'
TINT_SHA = '9afee921dd5312c1e81e344de1fbcd6fed9ddbc9f63d9b545b6d2ea91bc78ac9'

def fixtures():
    dust=struct.pack('<4I16f',17,1,0,0x100,283.25,-37.25,689.66,1,
                     0,0,0,0,1,1,1,0,0,0,0,0)
    colors=[(1,1,1,1,.5,.5,.5,1,1,1,1,0),
            (.2,.4,.6,1,.1,.2,.3,1,.7,.3,.1,0),
            (3,1,2,0,.25,.75,.5,0,.1,.2,.3,0),
            (0,0,0,0,0,0,0,0,1,0,0,0),
            (1.234,3.141,.9,1,.001,.005,.01,0,.3,.2,.7,0)]
    return dust,b''.join(struct.pack('<12f',*r) for r in colors)

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def compare_state(reference, native):
    if len(reference) != 33*425*4 or len(native) != len(reference):
        raise ValueError('Dust state interval extent')
    for step, row in enumerate(struct.iter_unpack('<425I', reference)):
        if row[:3] != (step,0,17) or row[4:6] != (9,30):
            raise ValueError('Dust state identity')
        if row[6] != (1 if step < 2 else min(step,31)):
            raise ValueError('Dust phase progression')
        if row[3] != (0 if step < 2 else 0x120 if step == 32 else 0x20):
            raise ValueError('Dust removal/update flags')
    if reference != native:
        raise ValueError('Dust state differs; all words require exact equality')
    return 33*425

def completed(directory, name):
    run = json.loads((directory/'original.run.json').read_text())
    if run['exit_code'] or digest(directory/name) != run['output_sha256']:
        raise ValueError('Incomplete or changed original probe')
    if digest(directory/('17.input.bin' if name.startswith('17.') else 'cases.bin')) != run['input_sha256']:
        raise ValueError('Original probe input changed')
    return run

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--reference', type=Path, default=Path('local/dust-probe-v1'))
    p.add_argument('--tint-reference', type=Path, default=Path('local/dust-tint-v1'))
    p.add_argument('--output', type=Path)
    p.add_argument('--prepare',type=Path,help='Only write reproducible original-probe inputs into a new directory')
    args = p.parse_args()
    dust_input,tint_input=fixtures()
    if args.prepare:
        args.prepare.mkdir(parents=True,exist_ok=False)
        write_exact(args.prepare/'17.input.bin',dust_input)
        write_exact(args.prepare/'tint.cases.bin',tint_input)
        return
    if not args.output:p.error('--output is required for an audit')
    args.output.mkdir(parents=True,exist_ok=False)
    if digest(Path('local/extracted/SCUS_971.29')) != ELF_SHA:
        raise ValueError('Different retail ELF')
    original_run = completed(args.reference,'17.original.bin')
    tint_run = completed(args.tint_reference,'original.bin')
    if digest(args.reference/'17.original.bin') != STATE_SHA:
        raise ValueError('Different reviewed dust corpus')
    if digest(args.reference/'17.original.bin.packets.bin') != PACKETS_SHA or digest(args.tint_reference/'original.bin') != TINT_SHA:
        raise ValueError('Different reviewed geometry/tint corpus')
    if (args.reference/'17.input.bin').read_bytes()!=dust_input or (args.tint_reference/'cases.bin').read_bytes()!=tint_input:
        raise ValueError('Different reviewed input cases')
    target = args.output/'native.bin'
    commands = [
        [str(args.native),'map-effect-probe','local/effect-input/CMNDATA/2D/TES.EFD',
         'local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF',str(args.reference/'17.input.bin'),'31',str(target)],
        [str(args.native),'effect-tint',str(args.tint_reference/'cases.bin'),str(args.output/'tint.bin')],
    ]
    for cmd in commands:
        subprocess.run(cmd,check=True)
    words = compare_state((args.reference/'17.original.bin').read_bytes(),target.read_bytes())
    vertices = compare_vertices((args.reference/'17.original.bin.packets.bin').read_bytes(),
                                Path(str(target)+'.vertices.bin').read_bytes(),17)
    if (args.output/'tint.bin').read_bytes() != (args.tint_reference/'original.bin').read_bytes():
        raise ValueError('Effect tint differs')
    files = [args.reference/'17.original.bin',args.reference/'17.original.bin.packets.bin',
             args.reference/'17.input.bin',args.tint_reference/'cases.bin',args.tint_reference/'original.bin',
             Path('local/effect-input/CMNDATA/2D/TES.EFD'),target,Path(str(target)+'.vertices.bin'),args.output/'tint.bin']
    report = dict(scope=__doc__,elf_sha256=ELF_SHA,exact_state_words=words,updates=31,
                  exact_source_vertices=vertices,exact_tint_cases=5,original_run=original_run,
                  original_tint_run=tint_run,native_commands=commands,native_binary_sha256=digest(args.native),
                  sha256={str(f):digest(f) for f in files})
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print('Dust constructor/31 updates,54 source vertices and five tint cases match exactly')

if __name__ == '__main__':main()
