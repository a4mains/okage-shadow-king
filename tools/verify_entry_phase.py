#!/usr/bin/env python3
"""Compare the whole village startup from observed event dispatch through handoff.

No fitted phase offset: original and native dispatch define frame zero. Actor
roots and manager state must match exactly. The initial camera target Y is one
float step different; the inactive retained blend rate also remains different.
"""
import argparse
import json
from pathlib import Path
import struct
from prepare_assets import ELF_HASH
from verify_event import digest, write_exact
from verify_message_opening import require, rows
from verify_interaction_scene import actor_rows
from verify_village_camera import REFERENCES

def number(word):
    return struct.unpack('<f',struct.pack('<I',word))[0]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a = p.parse_args()
    require(digest(Path('local/extracted/SCUS_971.29')) == ELF_HASH, 'Different retail ELF')
    camera,manager,dispatch,runs = {},{},[],{}
    for name,offset,transition_sha,camera_sha in REFERENCES:
        prefix = 'local/'+name
        runs[name] = json.loads(Path(prefix+'.run.json').read_text())
        require(runs[name]['exit_code'] == 0, 'Incomplete original capture')
        for suffix,sha,width,target in [('.transition.bin',transition_sha,128,manager),
                                        ('.camera.bin',camera_sha,32,camera)]:
            path = Path(prefix+suffix)
            require(digest(path) == sha, 'Changed original startup corpus')
            for r in rows(path.read_bytes(),width):
                if suffix == '.transition.bin':
                    if r[1] == 0x1bb7e0:
                        dispatch.append((r[0]+offset,r[2],r[3]))
                    if r[1] != 0x133a98:
                        continue
                frame = r[0]+offset
                require(frame not in target, 'Duplicate original frame')
                target[frame] = r
    require(dispatch == [(108,0x1326f8,60)], 'Different original startup wrapper')
    native = lambda suffix:Path(str(a.capture)+suffix)
    run_path = Path(str(a.capture).removesuffix('.png')+'.run.json')
    run = json.loads(run_path.read_text())
    require(run['exit_code'] == 0, 'Incomplete native capture')
    trace = {}
    for kind,suffix,width in [('camera','.camera.bin',16),('manager','.interaction.bin',8),
                              ('state','.camera-state.bin',5),('transition','.transition.bin',8),
                              ('filter','.screen-filter.bin',4)]:
        data = rows(native(suffix).read_bytes(),width)
        require([r[0] for r in data] == list(range(1,len(data)+1)), 'Native trace extent')
        trace[kind] = {r[0]:r for r in data}
    actors = actor_rows(native('.actor-1.bin'))
    require([r[0] for r in actors] == list(range(1,len(actors)+1)), 'Actor trace extent')
    entry = [r[0] for r in trace['transition'].values() if r[3] == 0 and r[6] == 1]
    require(len(entry) == 1, 'Expected one village entry')
    offset = entry[0]-dispatch[0][0]
    require(trace['manager'][entry[0]][4] == 1, 'Native startup did not dispatch at entry')
    first_clear = next(f for f in range(109,542) if not manager[f][35]&256)
    handoff = next(f for f in range(first_clear,542) if manager[f][17] == 0)
    require((first_clear,handoff) == (315,316), 'Changed original startup end')
    for f in range(109,handoff+1):
        r,g = manager[f],trace['manager'][f+offset-1]
        require((g[4],g[7],g[5],g[6]) ==
                (int(bool(r[35]&256)),r[36],r[48],r[118]), 'Entry manager differs at '+str(f))
        require(actors[f+offset-2][8:16] == r[40:48], 'Entry actor roots differ at '+str(f))
    differences,rate_differences = [],[]
    for f in range(108,handoff):
        r,g,state = camera[f],trace['camera'][f+offset],trace['state'][f+offset]
        expected = r[4:8]+r[12:16]+r[8:12]
        for i,(actual,want) in enumerate(zip(g[4:16],expected)):
            if actual == want:
                continue
            require(f == 108 and i == 5 and abs(actual-want) == 1,
                    'Unexpected camera difference at '+str(f))
            differences.append(dict(frame=f,native_frame=f+offset,component='target.y',
                original=number(want),native=number(actual),absolute_error=abs(number(actual)-number(want))))
        require((state[1],state[3]) == (r[2],r[3]), 'Camera mode/phase differs')
        if state[4] != r[28]:
            require(f < 190 and state[3] == 0x3f800000 and state[4] == 0 and r[28] == 0x3c23d70a,
                    'Active camera blend rate differs')
            rate_differences.append(f)
    filter_path = Path('local/reference-village-startup-300-v1.screen-filter.bin')
    require(digest(filter_path) == 'ef91bc6890e0e60249ee406efb88d3fc090ea615cf3b915bcd84d5c863bd6561',
            'Changed original startup filter corpus')
    draws = [r for r in rows(filter_path.read_bytes(),48) if r[1] == 0x1942c0 and r[0] >= 108]
    require([r[0] for r in draws] == list(range(108,190)), 'Startup feedback interval differs')
    for r in draws:
        require(r[7:10] == (1,0x49,2) and trace['filter'][r[0]+offset][1:] == (1,0x49,r[3]),
                'Startup filter state/alpha differs')
    report = dict(scope=__doc__,native_run=run,original_runs=runs,original_dispatch=108,
        native_dispatch=entry[0],elapsed_event_end=206,elapsed_control_return=207,
        manager_entries=208,exact_manager_words=832,actor_root_samples=208,exact_actor_words=1664,
        camera_updates=208,exact_camera_words=2496-len(differences),exact_mode_phase_words=416,
        feedback_updates=82,exact_feedback_words=246,
        camera_differences=differences,inactive_rate_difference_frames=rate_differences,
        native_event_end=first_clear+offset-1,native_control_return=handoff+offset-1)
    write_exact(a.output,(json.dumps(report,indent=2)+'\n').encode())
    print('Dispatch-aligned village startup:208 manager/actor samples exact; camera residuals recorded')

if __name__ == '__main__':
    main()
