#!/usr/bin/env python3
"""Audit the village's final startup camera move and event handoff.

The initial actor move starts one native frame late at destination construction.
Alignment here is explicitly the subsequent camera-mode0 transition, not an
assertion that dispatch-to-completion timing or whole-scene output matches.
"""
import argparse
import json
from pathlib import Path
from verify_event import digest, write_exact
from verify_message_opening import require, rows
from prepare_assets import ELF_HASH

REFERENCES = [
    ('reference-village-startup-300-v1', 0,
     '01126cf596a336fe5c92301e3c6dd16703454127023f44eb5cc0d63b0dce33d8',
     '9746717272216c4b3a7edbcc45ee4e6e048969e2e3b5727520f4cdad819eadb9'),
    ('reference-village-scene-2-v1', 300,
     '72137b990926c65a29c5e3053a505a47aecfe685fdf3bf69a47224f0ad7fbe19',
     '6de25d095256fdba20f6e9592a8c6c3497ddb4b5df51bba01dfaca52ea977040'),
    ('reference-village-startup-240-v1', 302,
     '937357aaf50777c0c9274e708b5ee17e4527a9fda4cf6a26b7b77bf2e302df23',
     'ed6097041d5c68f1ab6c89df5cdf86a7e0e2e2d46bff0edf626bebaa9f2a46b5')]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    require(digest(Path('local/extracted/SCUS_971.29')) == ELF_HASH, 'Different retail ELF')
    camera, manager, manifests = {}, {}, {}
    for name, offset, transition_sha, camera_sha in REFERENCES:
        prefix = Path('local')/name
        run = json.loads(Path(str(prefix)+'.run.json').read_text())
        require(run['exit_code'] == 0, 'Incomplete original run')
        manifests[name] = run
        for suffix, sha, width, target in [('.transition.bin',transition_sha,128,manager),
                                           ('.camera.bin',camera_sha,32,camera)]:
            path = Path(str(prefix)+suffix)
            require(digest(path) == sha, 'Different original corpus')
            for r in rows(path.read_bytes(), width):
                if target is manager and r[1] != 0x133a98:
                    continue
                frame = r[0]+offset
                require(frame not in target, 'Duplicate reference frame')
                target[frame] = r
    run = json.loads(Path(str(a.capture).removesuffix('.png')+'.run.json').read_text())
    require(run['exit_code'] == 0, 'Incomplete native capture')
    n = {}
    for kind, suffix, width in [('camera','.camera.bin',16), ('state','.camera-state.bin',5),
                                ('interaction','.interaction.bin',8)]:
        data = rows(Path(str(a.capture)+suffix).read_bytes(), width)
        require([r[0] for r in data] == list(range(1,len(data)+1)), 'Native trace extent')
        n[kind] = {r[0]: r for r in data}
    # Observed event boundaries, not a guessed offset through asynchronous loading.
    original_anchor = min(f for f in camera if f >= 108 and camera[f][2] == 0)
    native_anchor = min(f for f,r in n['camera'].items() if f >= 11461 and r[1] == 0x29f0)
    require((original_anchor,native_anchor) == (190,11544), 'Changed startup phase alignment')
    offset = native_anchor-original_anchor
    for f in range(190,315):
        r, got, state = camera[f], n['camera'][f+offset], n['state'][f+offset]
        require(got[4:8] == r[4:8] and got[8:12] == r[12:16] and got[12:16] == r[8:12], 'Camera values differ')
        require((state[1],state[3],state[4]) == (r[2],r[3],r[28]), 'Camera mode/blend differs')
    for f in range(190,317):
        r, got = manager[f], n['interaction'][f+offset-1]
        require((got[4],got[7],got[5],got[6]) ==
                (int(bool(r[35]&256)),r[36],r[48],r[118]), 'Manager/event handoff differs')
    exterior = Path('local/reference-exterior-camera-140-v1.camera.bin')
    require(digest(exterior) == 'f1f91f0c0ffd9d6ad45135497474c7f81fcc125fd12460cf81326621ba945366',
            'Different house-exit camera corpus')
    exterior_run = json.loads(Path('local/reference-exterior-camera-140-v1.run.json').read_text())
    require(exterior_run['exit_code'] == 0, 'Incomplete house-exit observation')
    for r in rows(exterior.read_bytes(),32)[:45]:
        got, state = n['camera'][5480+r[0]], n['state'][5480+r[0]]
        require((got[4:8],got[8:12],got[12:16]) == (r[4:8],r[12:16],r[8:12]),
                'House-exit camera correction differs')
        require((state[1],state[3]) == (r[2],r[3]), 'House-exit mode/phase differs')
        if r[0] >= 43:
            require(state[4] == r[28], 'Active house-exit blend rate differs')
    report = dict(scope=__doc__, original_runs=manifests, native_run=run,
        camera_updates=125, exact_camera_words=1500, exact_mode_blend_words=375,
        manager_entries=127, exact_manager_words=508, native_camera_anchor=native_anchor,
        house_exit_original_run=exterior_run, house_exit_camera_updates=45,
        house_exit_limit='Inactive blend rate differs before43; later follow target remains unvalidated.',
        original_camera_anchor=original_anchor, native_event_end=11668, native_control=11669,
        unresolved='Native destination construction defers first actor advance by one frame; no whole-startup timing or rendered/audio equivalence claimed.')
    write_exact(a.output, (json.dumps(report,indent=2)+'\n').encode())
    print('125 camera updates and127 manager observations exact after documented phase alignment')

if __name__ == '__main__':
    main()
