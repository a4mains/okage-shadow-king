#!/usr/bin/env python3
"""Audit native north-gate event state/camera and animated screen feedback.

State comparisons and executed GS packet observations do not establish raster,
audio, approach movement or whole-scene equivalence. Reference files are research
inputs only; native rendering retains its own completed framebuffer history.
"""
import argparse
import json
from pathlib import Path
import struct
from prepare_assets import ELF_HASH
from verify_event import digest, write_exact
from verify_message_opening import require, rows

HASHES = {
    "local/reference-north-gate-360-v1.interaction.bin": "fcee5e0c663eab68d94cac9a4a7acbf9f501c4667229408c42cc2b92a971162b",
    "local/reference-north-gate-360-v1.camera.bin": "0e06e49ec4262eb662a6ed0abc6d0d3fae1075e6d0be8abfb982dbdf115c75b7",
    "local/reference-north-gate-360-v1.screen-filter.bin": "9ba95427a355a5bd7431df5c8ef22aa952872f7164a7493399116502228fc637",
    "local/reference-north-gate-160-v2.camera.bin": "536ffe0b3f60cd81b0f6614dace0def9ac9c9f29094b836675af928757f665c2",
    "local/reference-north-gate-160-v2.screen-filter.bin": "a082504d475237ccadf9ec42c74b9d801e0113e0b7040332a2a413dc98948d9c",
    "local/reference-filter-packet-2-v1.screen-filter.bin.packets.bin": "b0a31ea5d40a61470d15910080429b6e53e52636329f6e75e945e7355e85a1b5"
}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--prefix', action='store_true', help='Audit the same fixed gate interval inside a longer complete native capture')
    a = p.parse_args()
    require(digest(Path('local/extracted/SCUS_971.29')) == ELF_HASH, 'Retail ELF changed')
    files = []
    for path, expected in HASHES.items():
        require(digest(Path(path)) == expected, 'Original corpus changed: ' + path)
        files.append(Path(path))
    prefix = 'local/reference-north-gate-360-v1'
    tail = 'local/reference-north-gate-160-v2'
    packet = 'local/reference-filter-packet-2-v1'
    runs = {}
    for name in [prefix, tail, packet]:
        path = Path(name + '.run.json')
        runs[name] = json.loads(path.read_text())
        require(runs[name]['exit_code'] == 0, 'Original run incomplete')
        files.append(path)
    native = lambda suffix: Path(str(a.capture) + suffix)
    run_path = Path(str(a.capture).removesuffix('.png') + '.run.json')
    run = json.loads(run_path.read_text())
    require(run['exit_code'] == 0, 'Native run incomplete')
    total = int(run['command'][run['command'].index('--frames') + 1])
    require(total >= 11460 if a.prefix else total == 11460,
            'Capture must cover the fixed interval through11460 (longer captures require --prefix)')
    require(run['command'][run['command'].index('--render-from') + 1] == '11310', 'Missing preceding native framebuffer')
    n = {key: rows(native(suffix).read_bytes(), width) for key, suffix, width in [
        ('interaction', '.interaction.bin', 8), ('camera', '.camera.bin', 16),
        ('state', '.camera-state.bin', 5), ('filter', '.screen-filter.bin', 4)]}
    for key, data in n.items():
        require([r[0] for r in data] == list(range(1, total + 1)), 'Native extent: ' + key)
    observed = rows(Path(prefix + '.interaction.bin').read_bytes(), 176)
    require([(r[0], r[3]) for r in observed if r[1] == 0x1bb7e0] == [(270, 1)], 'Gate event dispatch')
    manager = [r for r in observed if r[1] == 0x133a98]
    require([r[0] for r in manager] == list(range(360)), 'Original manager extent')
    for r in manager:
        got = n['interaction'][11039 + r[0]]
        require((got[4], got[7], got[5], got[6]) == (int(bool(r[20] & 256)), r[21], r[15], r[150]), 'Manager mismatch')
        require(n['interaction'][11040 + r[0]][1] == r[7] >> 16, 'Pressed latch mismatch')
    camera_count = filter_count = 0
    for name, offset, first, last in [(prefix, 11040, 270, 360), (tail, 11400, 0, 60)]:
        cameras = [r for r in rows(Path(name + '.camera.bin').read_bytes(), 32) if first <= r[0] < last]
        require([r[0] for r in cameras] == list(range(first, last)), 'Scripted camera extent')
        for r in cameras:
            got = n['camera'][offset + r[0]]
            require(got[4:8] == r[4:8] and got[8:12] == r[12:16] and got[12:16] == r[8:12], 'Scripted camera mismatch')
            state = n['state'][offset + r[0]]
            require((state[1], state[3], state[4]) == (r[2], r[3], r[28]), 'Camera mode/blend mismatch')
        camera_count += len(cameras)
        filters = rows(Path(name + '.screen-filter.bin').read_bytes(), 48)
        draws = [r for r in filters if r[1] == 0x1942c0 and first <= r[0] < last]
        require([r[0] for r in draws] == list(range(first, last)), 'Filter5 draw extent')
        for r in draws:
            require(r[7:10] == (1, 0x49, 2) and r[12:16] == (1, r[3], 0x40808080, 0), 'Filter5 state')
            require(n['filter'][offset + r[0]][1:] == (1, 0x49, r[3]), 'Native filter differs')
        configured = [r for r in filters if r[1] == 0x193c20 and r[2] == 0x1942e0 and first <= r[0] < last]
        require(len(configured) == last-first, 'Filter draw configuration extent')
        for r in configured:
            require(r[20:28] == (0xffffffff, 0x40808080, 640, 224, 1024, 512, 0x64, r[13]), 'Texture/ALPHA configuration')
        filter_count += len(draws)
    packets = rows(Path(packet + '.screen-filter.bin.packets.bin').read_bytes(), 64)
    require([r[0] for r in packets] == [0, 1], 'Executed packet extent')
    tex = []
    for r in packets:
        q = [(r[i] | r[i+1] << 32, r[i+2] | r[i+3] << 32) for i in range(4,44,4)]
        require(q[0] == (0x1000000000000009, 0xe), 'GIF A+D header')
        require(q[1] == (0x10100008c, 0x4e), 'ZBUF/depth write mask')
        require(q[2] == (0, 0x3f) and q[3][1] == 6, 'Texture flush/TEX0')
        require(q[4:] == [(0x40,0x14),(0x3ff003fff,8),(0x3000c,0x47),(0x6400000064,0x42),(0x40808080,1),(0x156,0)], 'GS filter configuration')
        tex.append(q[3][0])
    require(tex == [0x6a8028000,0x6a80288c0], 'Alternating source framebuffers')
    for suffix in ['.interaction.bin','.camera.bin','.camera-state.bin','.screen-filter.bin']:
        files.append(native(suffix))
    files += [a.capture, run_path]
    report = dict(scope=__doc__, elf_sha256=ELF_HASH, native_run=run, original_runs=runs,
                  native_capture_frames=total, audited_first_frame=11041, audited_last_frame=11460,
                  longer_capture_prefix=a.prefix,
                  event_frame=11311, manager_words=1440, pressed_latches=360,
                  camera_words=camera_count*12, camera_mode_blend_words=camera_count*3,
                  filter_frames=filter_count, packet_frames=2,
                  alpha_changes=[[11311,80],[11396,100],[11458,120]],
                  sha256={str(f):digest(f) for f in files})
    write_exact(a.output, (json.dumps(report, indent=2)+'\n').encode())
    print('North gate:360 manager/pressed,150 camera/filter updates and two executed GS packets verified')

if __name__ == '__main__':
    main()
