#!/usr/bin/env python3
"""Compare 32 static village sun/cloud transforms with two completed original frames.

Does not launch a VM, render the native village or establish whole-scene fidelity.
Captured camera and world matrices are diagnostic inputs only.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact
from verify_scene import compare, sha

DRAW_SHA = 'd70c3fcd4d8e2edbd561b93cd1c1e193080f4dda085b46d063d293b5f4e80e7b'
WORLD_SHA = '5af8c9a34105ee93a1c875ba35e9eb0534fbd778b14debb4e848330ace434f85'
BOOT_SHA = 'e453c2add0cd78d1de46e04a1f431322482401f75262bcd7bcc49f80891a5234'

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    p.add_argument('--reference', type=Path, default=Path('local/reference-village-scene-2-v1'))
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    run = json.loads(Path(str(args.reference)+'.run.json').read_text())
    if run['exit_code'] or run['binary_sha256'] != BOOT_SHA:
        raise ValueError('Incomplete or different original run')
    scene = Path(str(args.reference)+'.scene')
    if sha(scene/'draws.tsv') != DRAW_SHA:
        raise ValueError('Different original draw corpus')
    with (scene/'draws.tsv').open() as stream:
        draws = list(csv.DictReader(stream, delimiter='\t'))
    selected = [r for r in draws if int(r['model']) in (53, 54, 55)]
    if len(draws) != 324 or len(selected) != 32:
        raise ValueError('Incomplete original scene')
    cameras, worlds, snapshots = bytearray(), bytearray(), []
    for frame in (0, 1):
        rows = [r for r in selected if int(r['vblank']) == frame]
        if [int(r['model']) for r in rows] != [53]+[54]*12+[55]*3:
            raise ValueError('Billboard draw order differs from reviewed placements120..135')
        rotations = []
        for row in rows:
            stem = scene / ('%04d' % int(row['draw']))
            camera, world = (Path(str(stem)+suffix) for suffix in ('.camera.bin', '.world.bin'))
            if camera.stat().st_size != 48 or world.stat().st_size != 64:
                raise ValueError('Invalid camera/world extent')
            rotations.append(struct.unpack_from('<4f', camera.read_bytes(), 16))
            worlds.extend(world.read_bytes())
            snapshots.append(dict(draw=int(row['draw']), frame=frame,
                                  camera_sha256=sha(camera), world_sha256=sha(world)))
        if any(r != rotations[0] for r in rotations):
            raise ValueError('Camera changed within the billboard group')
        x, y, _, _ = rotations[0]
        cameras.extend(struct.pack('<4f', -x, -y, 0, 1))
    if hashlib.sha256(worlds).hexdigest() != WORLD_SHA:
        raise ValueError('Different reviewed world matrices')
    args.output.mkdir(parents=True, exist_ok=False)
    fixture, native = args.output/'camera.bin', args.output/'native.bin'
    write_exact(fixture, cameras)
    asset = Path('local/xpf-decoded/FLDDATA/MAP/VIL/VSTART_D.XPF/a.xmp')
    command = [str(args.native), 'map-facing-matrices', str(asset), str(fixture), str(native)]
    subprocess.run(command, check=True)
    report = compare(worlds, native.read_bytes())
    report.update(scope=__doc__, native_binary_sha256=sha(args.native), original_run=run,
                  command=command, map_sha256=sha(asset), camera_input_sha256=sha(fixture),
                  native_sha256=sha(native), original_world_sha256=WORLD_SHA, snapshots=snapshots,
                  tolerance='abs(original-native) <= 2e-5 + 2e-6*abs(original)')
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(f"32 billboard matrices compared; maximum error {report['max_absolute_error']:g}")

if __name__ == '__main__':
    main()
