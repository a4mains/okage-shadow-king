#!/usr/bin/env python3
"""Check HOME1B original phase order, player camera queries and native script state.

The 48-word observer is research-only. This does not establish raster, full pose,
NPC, effects or audio fidelity, nor compare requested and blended camera targets.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
from prepare_assets import ELF_HASH
from verify_event import digest,write_exact
from verify_interaction_scene import actor_rows
from verify_message_opening import rows,require

def expected_order(frame):
    return ([0x133a98]+([0x136aa8] if frame>=46 else [])+
            [0x1b5a90,0x1b9c74,0x1b70a0,0x102218]+
            ([0x101118] if frame>=102 else [])+
            ([0x134c18,0x134c68] if frame>=45 else []))

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    prefix='local/reference-home1b-camera-schedule-120-v2'
    ref=lambda suffix:Path(prefix+suffix)
    native=lambda suffix:Path(str(a.capture)+suffix)
    manifest=Path(str(a.capture).removesuffix('.png')+'.run.json')
    original_run=json.loads(ref('.run.json').read_text())
    native_run=json.loads(manifest.read_text())
    require(original_run['exit_code']==native_run['exit_code']==0,'Incomplete capture')
    require(digest(Path('local/extracted/SCUS_971.29'))==ELF_HASH,'Wrong ELF')
    require(original_run['command'][2]=='120' and
            'local/reference-home1b-date-420-v1.state.zip' in original_run['command'],'Reference continuity')
    schedule=rows(ref('.camera-schedule.bin').read_bytes(),48)
    camera=rows(ref('.camera.bin').read_bytes(),32)
    require(len(schedule)==842 and [r[0] for r in camera]==list(range(120)),'Original extent')
    hero=actor_rows(native('.actor-1.bin'))
    ncamera=rows(native('.camera.bin').read_bytes(),16)
    require(len(hero)==len(ncamera)>=18620,'Native extent')
    require([r[0] for r in hero]==list(range(1,len(hero)+1)),'Native sequence')
    require(all(0<=r[0]<120 for r in schedule),'Invalid original frame')
    require([r[0] for r in schedule]==sorted(r[0] for r in schedule),'Original order')
    for frame in range(120):
        samples=[r for r in schedule if r[0]==frame]
        require([r[1] for r in samples]==expected_order(frame),f'Phase sequence differs at{frame}')
        manager=samples[0]
        require(hero[18499+frame][1:4]==manager[44:47],f'Field/HERO script state differs at{frame}')
        if frame>=45:
            query=samples[-1]
            require(query[40:44]==camera[frame][8:12],f'Original queried camera differs at{frame}')
            require(query[40:44]==ncamera[18500+frame][12:16],f'Native movement camera differs at{frame}')
    unchanged=[]
    for suffix in ('.transition.bin','.camera.bin','.controller.bin'):
        old=Path('local/reference-home1b-camera-schedule-120-v1'+suffix)
        require(ref(suffix).read_bytes()==old.read_bytes(),'Observer changed '+suffix)
        unchanged.append(str(old))
    files=[manifest,ref('.run.json'),ref('.camera-schedule.bin'),ref('.camera.bin'),
           ref('.transition.bin'),ref('.controller.bin'),ref('.input'),
           native('.actor-1.bin'),native('.camera.bin')]+list(map(Path,unchanged))
    report=dict(scope=__doc__,elf_sha256=ELF_HASH,original_run=original_run,native_run=native_run,
                row_words=48,observations=len(schedule),frames=120,
                callbacks={hex(pc):n for pc,n in Counter(r[1] for r in schedule).items()},
                order=[dict(first=lo,last=hi,callbacks=list(map(hex,expected_order(lo))))
                       for lo,hi in ((0,44),(45,45),(46,101),(102,119))],
                exact_native_field_and_hero_script_words=120*3,
                exact_original_and_native_player_camera_query_words=75*4,
                unchanged_v1_files=unchanged,
                limits=['Native position and conversation comparisons are reported separately.',
                        'Original raw channel cache may be dirty; queried values are authoritative.',
                        '0x101118 observes an attachment callback, not an NPC callback.'],
                sha256={str(f):digest(f) for f in files})
    write_exact(a.output,(json.dumps(report,indent=2)+'\n').encode())
    print('Original phase sequences and native camera/script state validated:120 frames')

if __name__=='__main__':main()
