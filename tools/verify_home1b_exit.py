#!/usr/bin/env python3
"""Audit HOME1B departure and HOME1A entrance1 as separate loading phases.

Checks the observed input route, exit request/countdown, owned-data entrance and
preserved exit flag. Host synchronous loading is not original loading timing.
No whole-scene, animation-phase, raster or audio equivalence is claimed.
"""
import argparse
import json
import struct
from pathlib import Path

from prepare_assets import ELF_HASH
from verify_event import digest, write_exact
from verify_home1b_talk import compare_hero
from verify_interaction_scene import actor_rows
from verify_message_opening import require, rows
from verify_movement_scene import error

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    artifacts = []

    def path(prefix, suffix):
        result = Path(str(prefix) + suffix)
        artifacts.append(result)
        return result

    prefix = Path('local/reference-home1b-exit-240-v1')
    original_run = json.loads(path(prefix, '.run.json').read_text())
    native_run = json.loads(path(a.capture.with_suffix(''), '.run.json').read_text())
    require(original_run['exit_code'] == native_run['exit_code'] == 0, 'Incomplete capture')
    source = 'local/reference-home1b-father-close-120-v2.state.zip'
    require(source in original_run['command'], 'Original save continuity')
    log = path(prefix, '.log').read_text()
    require(f'Loaded reference state {source}' in log and
            'Stopped: vblanks=240 exit_request=0' in log, 'Original completion')
    require(digest(Path('local/extracted/SCUS_971.29')) == ELF_HASH, 'Different ELF')
    native = lambda suffix, width: rows(path(a.capture, suffix).read_bytes(), width)
    observed = rows(path(prefix, '.transition.bin').read_bytes(), 128)
    camera = rows(path(prefix, '.camera.bin').read_bytes(), 32)
    managers = native('.interaction.bin', 8)
    movement = native('.movement.bin', 16)
    cameras = native('.camera.bin', 16)
    states = native('.camera-state.bin', 5)
    transitions = native('.transition.bin', 8)
    actors = actor_rows(path(a.capture, '.actor-1.bin'))
    total = len(managers)
    require(total >= 21500 and all(len(r) == total for r in
            (movement, cameras, states, transitions, actors)), 'Native extent')
    require([r[0] for r in managers] == list(range(1, total + 1)), 'Native frame sequence')
    first = 21261
    before = [r for r in observed if r[1] == 0x133a98 and r[0] <= 69]
    require([r[0] for r in before] == list(range(70)), 'Departure manager extent')
    hero_error = camera_error = 0.
    for r in before:
        at = first + r[0] - 2
        n = managers[at]
        require(r[28] == 1, 'Missing departure player')
        require((n[4], n[7], n[5], n[6]) ==
                (int(bool(r[35] & 256)), r[36], r[48], r[118]), 'Departure event/mask/gate')
        require(managers[at + 1][1] == r[33] >> 16, 'Departure pressed latch')
        require(movement[at + 1][1] & 0xf000 == r[33] & 0xf000, 'Departure direction latch')
        for nw, ow in ((2, 48), (4, 49), (5, 50), (6, 51), (7, 52), (8, 53), (9, 54), (10, 55)):
            require(movement[at][nw] == r[ow], f'Player contact field{ow} at{r[0]}')
        hero_error = max(hero_error, compare_hero(actors[at][8:16], r[40:48], True))
    before_camera = [r for r in camera if r[0] < 69]
    require([r[0] for r in before_camera] == list(range(69)), 'Departure camera extent')
    for r in before_camera:
        at = first + r[0] - 1
        require(cameras[at][12:16] == r[8:12], 'Departure camera rotation')
        require((states[at][1], states[at][3]) == r[2:4], 'Departure camera mode/phase')
        camera_error = max(camera_error, error(cameras[at][4:8], r[4:8]))
    require(camera_error <= 0.00002, 'Departure camera position exceeds established bound')
    require([(r[0], r[3]) for r in observed if r[1] == 0x1bb7e0] == [(66, 1), (145, 50)],
            'Original event route')
    request, = [r for r in observed if r[1] == 0x124930]
    require(request[0] == 69 and request[4:7] == (2, 70, 1), 'Original scene request')
    requested = [r for r in transitions[first - 1:] if r[3] != 0xffffffff]
    require(requested == [(21327, 71, 0, 70, 1, 2, 0, 40),
                          (21328, 71, 0, 70, 1, 1, 0, 40),
                          (21329, 71, 0, 70, 1, 0, 0, 40),
                          (21330, 71, 0, 70, 1, 0, 1, 40)], 'Native request/countdown')
    arrival, = [r for r in observed if r[1] == 0x133a98 and r[0] == 146]
    require(arrival[21:24:2] == (70, 1) and arrival[28] == 1, 'Original entrance')
    require(actors[21329][8:16] == arrival[40:48], 'Owned STG entrance root')
    require(cameras[21329][1] == arrival[36] == 0x1b0, 'Entry50 completion')
    require(all(r[1:3] == (70, 1) for r in transitions[21330:]), 'Destination continuity')
    require(all(r[2] == 0 for r in movement[21329:]), 'Destination player remains gated')
    variables = path(a.capture, '.variables.bin').read_bytes()
    require(len(variables) == 0x1000 and variables[0x111] & 0x80, 'Father exit flag lost')
    require(not any(variables[0x10:0x110]), 'Temporary field bits not reset')
    require(struct.unpack_from('<2I', variables, 0x510) == (70, 1), 'Scene/entrance variables')
    report = dict(scope=__doc__, elf_sha256=ELF_HASH, native_run=native_run,
                  original_run=original_run, native_first_frame=first,
                  departure_manager_samples=70, departure_camera_samples=69,
                  hero_position_maximum_error=hero_error, camera_position_maximum_error=camera_error,
                  position_tolerance=0.00002, native_request_frame=21327,
                  original_request_frame=66, native_load_frame=21330,
                  original_startup_frame=145, original_first_arrival_manager=146,
                  destination_scene=70, destination_entrance=1, bank1_bit8=1,
                  arrival_root_exact=True, full_loading_timeline_equivalence=False,
                  limits=['Camera targets have the previously documented requested/blended distinction.',
                          'Arrival is compared by lifecycle, independently of original asynchronous loading.',
                          'No claim about post-arrival animation phases, pixels or audio.'],
                  sha256={str(f): digest(f) for f in artifacts})
    write_exact(a.output, (json.dumps(report, indent=2) + '\n').encode())
    print('HOME1B departure and HOME1A entrance1 validated within stated scopes')

if __name__ == '__main__':
    main()
