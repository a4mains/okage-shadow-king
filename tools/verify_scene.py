#!/usr/bin/env python3
"""Bounded opening-scene pose and instance-matrix comparisons, not playability."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def compare(retail, native, active=None):
    if not retail or len(retail) % 64 or len(retail) != len(native):
        raise ValueError('Matrix buffer extent mismatch')
    a = struct.unpack('<%df' % (len(retail)//4), retail)
    b = struct.unpack('<%df' % (len(native)//4), native)
    nodes = list(range(len(retail)//64)) if active is None else list(active)
    if not nodes or len(set(nodes)) != len(nodes) or any(i < 0 or i >= len(retail)//64 for i in nodes):
        raise ValueError('Invalid matrix selection')
    maximum = 0.0
    for node in nodes:
        for j in range(16):
            i = node*16+j
            if not math.isfinite(a[i]) or not math.isfinite(b[i]):
                raise ValueError('Non-finite selected matrix')
            error = abs(a[i]-b[i])
            if error > 2e-5 + 2e-6*abs(a[i]):
                raise ValueError('Matrix %d component %d differs: %r vs %r' % (node, j, a[i], b[i]))
            maximum = max(maximum, error)
    return {'matrices': len(nodes), 'components': len(nodes)*16, 'max_absolute_error': maximum}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--snapshot', type=Path, default=Path('local/reference-scene-602-v2'))
    parser.add_argument('--map-report', type=Path, default=Path('local/map-validation/report.json'))
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--oracle', type=Path, default=Path('build/play-research/okage-oracle/okage-pose-oracle'))
    parser.add_argument('--elf', type=Path, default=Path('local/extracted/SCUS_971.29'))
    parser.add_argument('--output', type=Path, default=Path('local/scene-validation'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    map_report = json.loads(args.map_report.read_text())
    cases = bytearray()
    for i, row in enumerate(map_report['records']):
        path = args.map_report.parent / ('%04d.native.bin' % i)
        if sha(path) != row['transform_sha256']:
            raise ValueError('Map fixture hash changed')
        data = path.read_bytes()
        if len(data) != row['placements']*52:
            raise ValueError('Map transform fixture extent mismatch')
        for at in range(0, len(data), 52):
            cases.extend(data[at:at+48])
    fixture = args.output / 'instances.bin'
    if fixture.exists() and fixture.read_bytes() != cases:
        raise ValueError('Refusing changed instance fixture')
    fixture.write_bytes(cases)
    native = args.output / 'instances.native.bin'
    retail = args.output / 'instances.retail.bin'
    subprocess.run([str(args.native), 'instance-matrices', str(fixture), str(native)], check=True)
    subprocess.run([str(args.oracle), 'instance-matrices', str(args.elf), str(fixture), str(retail)], check=True)
    instances = compare(retail.read_bytes(), native.read_bytes())
    instances.update({'fixture_sha256': sha(fixture), 'retail_sha256': sha(retail), 'native_sha256': sha(native)})
    print('Verified %d instance matrices' % instances['matrices'], flush=True)
    camera_cases=bytes(cases)+b''.join(path.read_bytes() for path in sorted(args.snapshot.glob('*.camera.bin')))
    fixture=args.output/'cameras.bin'
    if fixture.exists() and fixture.read_bytes()!=camera_cases:
        raise ValueError('Refusing changed camera fixture')
    fixture.write_bytes(camera_cases)
    native=args.output/'cameras.native.bin';retail=args.output/'cameras.retail.bin'
    subprocess.run([str(args.native),'camera-matrices',str(fixture),str(native)],check=True)
    subprocess.run([str(args.oracle),'camera-matrices',str(args.elf),str(fixture),str(retail)],check=True)
    cameras=compare(retail.read_bytes(),native.read_bytes())
    cameras.update({'fixture_sha256':sha(fixture),'retail_sha256':sha(retail),'native_sha256':sha(native)})
    print('Verified %d camera matrices' % cameras['matrices'],flush=True)
    room = Path('local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF')
    actor = Path('local/xpf-decoded/FLDDATA/OBJ/MAM.XPF')
    files = {2: room/'wal.xmd', 3: room/'nun1.xmd', 4: room/'kum.xmd', 5: room/'lmp.xmd',
             6: room/'kag1.xmd', 7: room/'tubo.xmd', 8: actor/'a.xmd', 9: actor/'shadow.xmd'}
    rows = []
    with (args.snapshot/'draws.tsv').open() as stream:
        draws = list(csv.DictReader(stream, delimiter='\t'))
    if len(draws) != 16:
        raise ValueError('Expected two opening-frame samples of eight model draws')
    for row in draws:
        i = int(row['draw'])
        if int(row['scale_mode']) != 0:
            raise ValueError('Unvalidated hierarchy scale mode')
        prefix = args.snapshot / ('%04d' % i)
        model = files[int(row['model'])]
        native = args.output / ('%04d.pose.bin' % i)
        subprocess.run([str(args.native), 'snapshot-pose', str(model), str(prefix), str(native)], check=True)
        hierarchy = Path(str(prefix)+'.hierarchy.bin').read_bytes()
        nodes = int(row['nodes'])
        if len(hierarchy) != 8+nodes*8 or struct.unpack_from('<I', hierarchy)[0] != nodes:
            raise ValueError('Hierarchy extent mismatch')
        # Original 0x184a20 skips nodes with bit 0x40 in this source flag.
        # Unused matrix storage can contain stale bytes; it is not a reference.
        active = [n for n in range(nodes) if not struct.unpack_from('<H', hierarchy, 14+n*8)[0] & 0x40]
        retail = Path(str(prefix)+'.retail-pose.bin')
        result = compare(retail.read_bytes(), native.read_bytes(), active)
        result.update({'draw': i, 'model': str(model), 'model_sha256': sha(model),
                       'native_sha256': sha(native), 'retail_sha256': sha(retail),
                       'skipped_unrequested_nodes': nodes-len(active),
                       'snapshot_sha256': {suffix: sha(Path(str(prefix)+suffix)) for suffix in
                           ['.world.bin', '.view.bin', '.camera.bin', '.bones.bin', '.hierarchy.bin', '.projection.bin']}})
        rows.append(result)
    report = {'scope': 'Instance matrices and selected model-to-view pose matrices for two captured opening frames. '
              'Not a native event/camera/actor update or full rendered-frame comparison.',
              'elf_sha256': sha(args.elf), 'native_sha256': sha(args.native), 'oracle_sha256': sha(args.oracle),
              'tolerance': 'abs(retail-native) <= 2e-5 + 2e-6*abs(retail)', 'instances': instances,'cameras':cameras,
              'snapshot': str(args.snapshot), 'draws_sha256': sha(args.snapshot/'draws.tsv'),
              'scene_matrices': sum(r['matrices'] for r in rows),
              'scene_max_absolute_error': max(r['max_absolute_error'] for r in rows), 'draws': rows}
    (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print('Verified %d selected scene matrices; max error %.9g' %
          (report['scene_matrices'], report['scene_max_absolute_error']))

if __name__ == '__main__':
    main()
