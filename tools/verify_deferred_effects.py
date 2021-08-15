#!/usr/bin/env python3
"""Audit original deferred scheduling and native flame16 drawing, not scene integration."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_map_effect import digest, ELF_SHA
from verify_torch_drawing import fixture, compare_matrices, compare_vertices

MANAGER, ORDINARY, DEFERRED, NPC, PLAYER, FOLLOW, UPDATE = (
    0x133a98, 0x102218, 0x102cf8, 0x138130, 0x134c18, 0x101118, 0x1131d0)
CASES = {'home1b': (30, 5), 'contact': (45, 2), 'summon': (30, 8)}

def require(value, message):
    if not value:
        raise ValueError(message)

def audit_order(data, case):
    require(case in CASES, 'Unknown effect-order case')
    require(len(data) and len(data) % 400 == 0, 'Effect-order row extent')
    rows = list(struct.iter_unpack('<100I', data))
    frames, npcs = CASES[case]
    require(sorted(set(r[0] for r in rows)) == list(range(frames)), 'Missing effect-order frames')
    require([r[0] for r in rows] == sorted(r[0] for r in rows), 'Nonchronological frames')
    updates = follows = 0
    for frame in range(frames):
        group = [r for r in rows if r[0] == frame]
        ordinary = ([(i,91) for i in range(3,32)]+[(32,19)] if case == 'home1b' else
                    [(i,91) for i in range(24)]+[(24,19),(25,19)] if case == 'contact' else
                    [(i,91) for i in range(28)])
        deferred = ([(i,16) for i in range(3)] if case == 'home1b' else
                    [(26,30)] if case == 'contact' and frame >= 24 else
                    [] if case == 'contact' else [(i,30) for i in range(29,34)]+[(34,115)])
        expected = [MANAGER, ORDINARY]+[UPDATE]*len(ordinary)+[DEFERRED]
        for slot, identity in deferred:
            expected += [FOLLOW, UPDATE] if identity == 30 else [UPDATE]
        expected += [NPC]*npcs + ([PLAYER] if case == 'contact' else [])
        require([r[1] for r in group] == expected, f'Effect callback order differs at {case}:{frame}')
        queue = tuple(slot for slot, identity in deferred)
        active_pass = ORDINARY
        expected_updates = iter(ordinary+deferred)
        for at, r in enumerate(group):
            require(r[4] <= 84 and all(v == 0xffffffff for v in r[16+r[4]:]), 'Invalid queue tail')
            if r[1] == DEFERRED:
                active_pass = DEFERRED
                require(r[16:16+r[4]] == queue, 'Deferred queue identity differs')
                require(r[2] == 0x1012d8, 'Deferred caller differs')
            if r[1] == ORDINARY:
                require(r[2] == 0x1012c4, 'Ordinary caller differs')
            if r[1] in (MANAGER, ORDINARY, NPC, PLAYER):
                require(r[4] == 0, 'Queue not cleared outside effect passes')
            if r[1] == FOLLOW:
                require(r[2] == 0x102e4c and r[7] == 30 and r[11] == FOLLOW, 'Follow callback identity differs')
                require(group[at+1][5:13] == r[5:13], 'Follow must immediately precede same-job update')
                follows += 1
            if r[1] == UPDATE:
                require((r[5],r[7]) == next(expected_updates), 'Effect slot/update order differs')
                require(r[2] == (0x1024d8 if active_pass == ORDINARY else 0x102e54), 'Update caller differs')
                require(bool(r[8]&0x400) == (active_pass == DEFERRED), 'Wrong effect pass flags')
                require(not r[8]&0x4300, 'Removed/disabled effect updated')
                if active_pass == DEFERRED:
                    require(r[16:16+r[4]] == queue, 'Queue mutated during deferred pass')
                updates += 1
    return {'frames': frames, 'rows':len(rows), 'effect_updates': updates,
            'follow_callbacks': follows, 'sequence_exact':True}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native-release/okage-assets'))
    p.add_argument('--reference', type=Path, default=Path('local/flame16-drawing-reference-v1'))
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    require(digest(Path('local/extracted/SCUS_971.29')) == ELF_SHA, 'Wrong ELF')
    source, original = args.reference/'input.bin', args.reference/'original.bin'
    packets = Path(str(original)+'.packets.bin')
    run = json.loads((args.reference/'original.run.json').read_text())
    require(run['exit_code'] == 0 and run['elf_sha256'] == ELF_SHA, 'Incomplete original draw probe')
    require(source.read_bytes() == fixture(True), 'Flame16 fixture differs')
    for path, key in [(source,'input_sha256'),(original,'output_sha256'),(packets,'packets_sha256')]:
        require(digest(path) == run[key], 'Drawing provenance differs')
    require(digest(original) == 'da1dcbf20320b54a487e4d7ecac7348959fb3356fe1716fcf7dfd6afc65fa9eb', 'Unreviewed original matrices')
    require(digest(packets) == '937b4239dd7dd0447ccdc4735024346162b7fc36c22e4f129395fc39a136655e', 'Unreviewed original geometry')
    orders = {}
    for case,(frames,_) in CASES.items():
        prefix = Path(f'local/reference-{case}-effect-order-{frames}-v1')
        record = json.loads(Path(str(prefix)+'.run.json').read_text())
        require(record['exit_code'] == 0, 'Incomplete effect-order capture')
        trace = Path(str(prefix)+'.bin')
        orders[case] = dict(audit_order(trace.read_bytes(),case), run=record, trace_sha256=digest(trace))
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output/'native.bin'
    command = [str(args.native),'flame16-effect-geometry','local/effect-input/CMNDATA/2D/TES.EFD',
               'local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF',str(source),str(output)]
    result = subprocess.run(command,capture_output=True)
    write_exact(args.output/'native.log',result.stdout+result.stderr)
    write_exact(args.output/'native.run.json',(json.dumps(dict(command=command,exit_code=result.returncode,
                binary_sha256=digest(args.native)),indent=2)+'\n').encode())
    result.check_returncode()
    error = compare_matrices(original.read_bytes(),output.read_bytes(),True)
    vertices = compare_vertices(packets.read_bytes(),Path(str(output)+'.vertices.bin').read_bytes(),True)
    report = dict(scope=__doc__,elf_sha256=ELF_SHA,orders=orders,original_draw_run=run,
                  native_command=command,native_binary_sha256=digest(args.native),efd_sha256=digest(Path(command[2])),
                  drawing=dict(cases=5,transforms=15,colors_exact=True,matrix_max_error=error,
                               matrix_tolerance=2e-5,source_vertices_exact=vertices,
                               material=dict(alpha='0x48',zbuf_mask=1,test='0x53001')))
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print(f'105 original frames audited; {vertices} flame16 vertices exact; 15 transforms maximum error {error:g}')

if __name__ == '__main__':
    main()
