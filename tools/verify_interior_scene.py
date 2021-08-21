#!/usr/bin/env python3
"""Audit native interior allocation/submission order and retained route outputs.

Does not establish original raster equality, whole-scene RNG/phase, general cold
startup, or summoning geometry. Original RAM remains auditor-only input.
"""
import argparse
import collections
import json
import mmap
from pathlib import Path
import struct
from verify_deferred_effects import UPDATE, audit_order, require
from verify_event import write_exact
from verify_map_effect import digest, ELF_SHA
from verify_effect_scene import effect_rows

def submission_rows(data):
    at, rows = 0, []
    while at < len(data):
        require(len(data)-at >= 8, 'Submission header extent')
        frame,count = struct.unpack_from('<2I',data,at);at += 8
        require(frame == len(rows)+1 and count <= 84, 'Submission frame/count')
        require(len(data)-at >= count*16, 'Submission row extent')
        jobs = list(struct.iter_unpack('<4I',data[at:at+count*16]));at += count*16
        require(len({r[0] for r in jobs}) == count and all(r[0]<84 for r in jobs), 'Duplicate/outside submission handle')
        require(all(not r[2]&0x4300 for r in jobs), 'Removed/disabled submission')
        require(jobs == sorted(jobs,key=lambda r:(bool(r[2]&0x400),r[0])), 'Submission pass/pool order')
        rows.append(jobs)
    return rows

def compare_order(native, original, first_frame, case):
    audit_order(original,case)
    rows = list(struct.iter_unpack('<100I',original))
    phase_mismatches = collections.Counter()
    matched = bubble_phases = 0
    for frame in sorted({r[0] for r in rows}):
        updates = [r for r in rows if r[0]==frame and r[1]==UPDATE]
        got = native[first_frame+frame-1]
        require([(r[0],r[1],r[2]) for r in got] == [(r[5],r[7],r[8]|0x20) for r in updates], 'Original/native submission identity/flags/order differs')
        for ref, row in zip(updates,got):
            phase = ref[9]-(ref[10] if ref[9]>ref[10] else 0)+(0 if ref[8]&0x2000 else 1)
            if row[1]==30:
                require(row[3]==phase, 'Contact bubble phase differs')
                bubble_phases += 1
            elif row[3]!=phase:
                phase_mismatches[str(row[1])] += 1
            matched += 1
    return dict(frames=len({r[0] for r in rows}),submissions_exact_except_map_phase=matched,
                contact_phases_exact=bubble_phases,unresolved_map_phase_mismatches=dict(phase_mismatches))

def selected_map_headers(path,frames):
    result={};previous=0
    with path.open('rb') as f, mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
        at=0
        while at<len(data):
            require(len(data)-at>=8,'Map trace header extent')
            frame,count=struct.unpack_from('<2I',data,at);at+=8
            require(frame==previous+1 and count<=84,'Map trace frame/count');previous=frame
            jobs={}
            for _ in range(count):
                require(len(data)-at>=88,'Map job extent')
                row=struct.unpack_from('<22I',data,at);at+=88
                require(row[0]<84 and row[0] not in jobs and row[5]<=128,'Map job identity')
                require(len(data)-at>=row[5]*80,'Map elements extent');at+=row[5]*80
                jobs[row[0]]=row
            if frame in frames:result[frame]=jobs
    require(set(result)==set(frames),'Missing selected map frame')
    return result

def compare_placement_headers(jobs,ram):
    require(len(ram)>=32*1024*1024,'Original RAM extent')
    def word(at):
        require(0<=at<=32*1024*1024-4,'Original RAM bounds')
        return struct.unpack_from('<I',ram,at)[0]
    table=word(0x1f849c)
    for slot, native in jobs.items():
        job=word(table+slot*4)
        require(job<=32*1024*1024-128,'Original job missing')
        original=(slot,word(job),word(job+4),word(job+0x64),word(job+0x60),word(job+8))+struct.unpack_from('<16I',ram,job+0x10)
        require(native[:3]+native[4:]==original[:3]+original[4:],'Placement identity/header/transforms/color differs')
    return len(jobs)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();prefix=str(a.capture)
    run=json.loads(Path(prefix.removesuffix('.png')+'.run.json').read_text())
    require(run['exit_code']==0 and digest(Path('local/extracted/SCUS_971.29'))==ELF_SHA,'Incomplete run or wrong ELF')
    submissions=submission_rows(Path(prefix+'.effect-submissions.bin').read_bytes())
    require(len(submissions)==47855,'Expected complete47855 route')
    cases={}
    for case,frames,first in [('contact',45,5050),('home1b',30,18501)]:
        original=Path(f'local/reference-{case}-effect-order-{frames}-v1.bin')
        cases[case]=dict(compare_order(submissions,original.read_bytes(),first,case),
                         native_first_frame=first,reference=str(original),reference_sha256=digest(original))
    sources={5074:'local/research/kitchen-end700-v2-ee.bin',
             18501:'local/research/home1b360-v1-ee.bin',
             47855:'local/research/basement-summon-v1/after-chant.ram'}
    headers=selected_map_headers(Path(prefix+'.map-effects.bin'),sources)
    placements=[]
    for frame,source in sources.items():
        count=compare_placement_headers(headers[frame],Path(source).read_bytes())
        placements.append(dict(frame=frame,jobs=count,source=source,source_sha256=digest(Path(source))))
    require([p['jobs'] for p in placements]==[26,33,28],'Wrong interior pool extents')
    baseline=Path('local/native-deferred-drawing-regression-47855-release-v1.png')
    changed={'.png','.effects.bin','.map-effects.bin','.movement.bin'}
    preserved=[]
    for old in sorted(baseline.parent.glob(baseline.name+'*')):
        suffix=old.name[len(baseline.name):]
        if suffix in changed or not suffix:continue
        current=Path(prefix+suffix)
        require(digest(old)==digest(current),'Unexpected route regression: '+suffix)
        preserved.append(suffix)
    require(len(preserved)==33,'Historical output family changed')
    old=Path(str(baseline)+'.movement.bin').read_bytes();new=Path(prefix+'.movement.bin').read_bytes()
    require(len(old)==len(new)==47855*64,'Movement extent')
    handle_changes=0
    for x,y in zip(struct.iter_unpack('<16I',old),struct.iter_unpack('<16I',new)):
        require(x[:15]==y[:15],'Movement changed beyond shared contact handle')
        handle_changes+=x[15]!=y[15]
    old_effects=effect_rows(Path(str(baseline)+'.effects.bin'))
    new_effects=effect_rows(Path(prefix+'.effects.bin'))
    require(len(old_effects)==len(new_effects)==47855,'Attached-effect frame extent')
    bubble_handle_changes=0
    for before,after in zip(old_effects,new_effects):
        require(before[:2]==after[:2] and len(before[2])==len(after[2]),'Attached-effect lifecycle changed')
        for x,y in zip(before[2],after[2]):
            require(x[1:]==y[1:],'Attached state changed beyond shared handle')
            bubble_handle_changes+=x[0]!=y[0]
    report=dict(scope=__doc__,elf_sha256=ELF_SHA,native_run=run,order=cases,
                placements=placements,placement_words_exact=87*21,preserved_outputs=preserved,
                contact_handle_changed_frames=handle_changes,
                attached_handle_changed_observations=bubble_handle_changes,
                excluded_from_equivalence=['Original raster/audio fidelity','Full scene RNG and map phases','Summoning33/115 geometry'])
    a.output.mkdir(parents=True,exist_ok=False)
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print('87 placement headers exact excluding phase;75 submission frames match;33 route outputs preserved')

if __name__=='__main__':main()
