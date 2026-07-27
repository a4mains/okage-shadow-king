"""Restore reconstruction audit (read-only research tool; never product input).

restore-check proves payload equality, transitions and continued play. This
audit adds the reconstruction the handoff names for a cold Restore:

  * field: the saved scene/entrance words (payload 0x510/0x514, read by the
    Restore callback in src/native_runtime.mm) are the ones the ordinary loader
    queued and activated after `Native Restore requested saved field`;
  * pose: the restored hero (actors-state alias -1, rotation channel 0 and
    position channel 1) equals the scene's STG entrance record
    (src/field.cpp field_entrance), read independently from the owned stage via
    FLDSET.XDB scene -> room;
  * resource bindings: with --newgame-report, the field-world report at
    `field-activation-return` of the Restore load and of a later ordinary load
    of the same scene in the same process bind the same content: per world
    model, per XTA scene image and per handle, the source image name and the
    SHA-256 of every byte upload into the allocation bound at each level
    (state.json handle -> record/allocations, timeline `upload-bytes`), plus
    actor registrations and model
    group names. Handle numbers and VRAM placement differ by history and are
    not compared.

Exit 0 when every check passes, 1 otherwise, 2 for malformed input.
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

SCENE_WORD, ENTRANCE_WORD = 0x510, 0x514
REQUESTED = 'Native Restore requested saved field'
QUEUED = re.compile(r'^Native ordinary field queued scene=(-?\d+) entrance=(-?\d+) frame=(\d+)$')
ACTIVATED = re.compile(r'^Native ordinary field activated scene=(-?\d+) entrance=(-?\d+) frame=(\d+)')
POSE_TOLERANCE = 1e-4

class Malformed(Exception):
    pass

def u32(b, at):
    if at + 4 > len(b):
        raise Malformed(f'read past end at {at:#x}')
    return struct.unpack_from('<I', b, at)[0]

def xdb_scene(data, scene):
    """FLDSET.XDB scene record (src/field.cpp read_definition)."""
    if u32(data, 0) != 0x424458 or u32(data, 16) != 0xffffffff:
        raise Malformed('invalid field XDB header')
    count = u32(data, 12)
    base = 32 + 16 * count
    strings = base + u32(data, 4)
    if count > 65536 or strings + u32(data, 8) != len(data):
        raise Malformed('field XDB section bounds')

    def string(offset):
        at = strings + offset
        end = data.index(0, at)
        return data[at:end].decode('ascii')

    found = None
    for i in range(count):
        row = 32 + i * 16
        if string(u32(data, row)) != f'scene{scene}':
            continue
        if found is not None:
            raise Malformed('ambiguous field scene record')
        at = base + u32(data, row + 4)
        found = {'room_directory': string(u32(data, at + 16)), 'room': string(u32(data, at + 20)),
                 'music': struct.unpack_from('<i', data, at + 40)[0]}
    if found is None:
        raise Malformed(f'no FLDSET.XDB record for scene {scene}')
    return found

def stg_entrance(stg, entrance):
    """STG entrance record (src/field.cpp field_entrance): 28-byte rows,
    position floats at +4, rotation floats at +16."""
    if u32(stg, 0) != 0x475453:
        raise Malformed('invalid STG magic')
    start, count = u32(stg, 24), u32(stg, 28)
    if start < 32 or start & 3 or count > 30 or entrance < 0 or entrance >= count or start + count * 28 > len(stg):
        raise Malformed('invalid STG entrance table/index')
    v = struct.unpack_from('<6f', stg, start + entrance * 28 + 4)
    return list(v[:3]), list(v[3:])

def restore_field(stdout_lines):
    """(queued, activated) (scene, entrance) of the first load after the Restore request."""
    try:
        i = next(n for n, l in enumerate(stdout_lines) if l.startswith(REQUESTED))
    except StopIteration:
        raise Malformed('no Restore saved-field request in the native log')
    queued = activated = None
    for l in stdout_lines[i + 1:]:
        m = QUEUED.match(l)
        if m and queued is None:
            queued = (int(m[1]), int(m[2]))
            continue
        m = ACTIVATED.match(l)
        if m and queued is not None:
            activated = (int(m[1]), int(m[2]))
            break
    return queued, activated

def as_float(word):
    return struct.unpack('<f', struct.pack('<I', int(word, 16) if isinstance(word, str) else word))[0]

def hero_pose(actors_state):
    hero = [a for a in actors_state['actors'] if a['alias'] == -1]
    if len(hero) != 1:
        raise Malformed('restored actors-state lacks exactly one hero')
    w = [as_float(x) for x in hero[0]['raw_channel_words'][:8]]
    return w[4:7], w[0:3]  # channel 1 position, channel 0 rotation

def close(a, b):
    return all(abs(x - y) <= POSE_TOLERANCE for x, y in zip(a, b)) and len(a) == len(b)

def bindings(checkpoint_dir):
    """Content-identified resource bindings at a field-world report checkpoint.
    state.json maps each live handle to its record (source image name) and to
    its image and palette allocation ranges; allocation ids are never reused,
    so the timeline's upload-bytes SHA-256s identify the bytes in them."""
    d = Path(checkpoint_dir)
    field = json.loads((d / 'field.json').read_text())
    world = json.loads((d / 'world.json').read_text())
    groups = json.loads((d / 'model-groups.json').read_text())
    state = json.loads((d / 'state.json').read_text())
    uploads = {}
    for line in (d / 'timeline.jsonl').read_text().splitlines():
        e = json.loads(line)
        if e['kind'] == 'upload-bytes':
            uploads.setdefault(e['alloc'], []).append(e['sha256'])
    records = {r['record']: r['name'] for r in state['records']}
    handles = {h['handle']: h for h in state['handles']}

    def content(handle):
        h = handles.get(handle)
        if h is None:
            return {'name': None, 'image': [], 'palettes': []}
        return {'name': records.get(h['record'], ''),
                'image': [uploads.get(a, []) for a in range(*h['image'])] if h['image'] else [],
                'palettes': [[uploads.get(a, []) for a in range(*p)] if p else None for p in h['palettes'] or []]}

    return {'scene': field['scene'], 'entrance': field['entrance'],
            'models': [{'name': m['name'], 'handles': [content(h) for h in m['handles']]} for m in world['models']],
            'xta_scene': [content(h) for h in world['xta_scene']],
            'actors': sorted(a['name'] for a in groups['actor_registrations']),
            'groups': [g['name'] for g in groups['groups']]}

def compare_bindings(a, b):
    """Differences between two bindings() results, ignoring the entrance."""
    out = []
    if a['scene'] != b['scene']:
        out.append({'what': 'scene', 'a': a['scene'], 'b': b['scene']})
    for key in ('models', 'xta_scene', 'actors', 'groups'):
        if a[key] != b[key]:
            first = next((i for i, (x, y) in enumerate(zip(a[key], b[key])) if x != y), min(len(a[key]), len(b[key])))
            out.append({'what': key, 'index': first, 'a_len': len(a[key]), 'b_len': len(b[key]),
                        'a': a[key][first] if first < len(a[key]) else None,
                        'b': b[key][first] if first < len(b[key]) else None})
    unbound = [m['name'] for m in a['models'] for h in m['handles']
               if h['name'] is None or not h['image'] or not all(h['image'])]
    if unbound:
        out.append({'what': 'restored model handle without uploaded content', 'models': sorted(set(unbound))})
    return out

def report_loads(report_root):
    """Every field-activation-return checkpoint under a --newgame-report root,
    in field resource cycle order: (cycle, path, scene, entrance)."""
    root = Path(report_root)
    found = []
    cycles = [p for p in root.glob('field-*') if p.is_dir() and re.fullmatch(r'field-\d+', p.name)]
    for d in [root] + sorted(cycles, key=lambda p: int(p.name.split('-')[1])):
        cp = d / 'field-activation-return'
        if (cp / 'field.json').is_file():
            f = json.loads((cp / 'field.json').read_text())
            cycle = 1 if d == root else int(d.name.split('-')[1])
            found.append((cycle, cp, f['scene'], f['entrance']))
    return found

def audit(run, stage, save_file, report_root=None):
    run, stage = Path(run), Path(stage)
    checks = {}
    save = Path(save_file).read_bytes()
    payload = save[12:]  # '!crc', length, checksum, then the 0xd78-byte payload (savefmt.py)
    scene, entrance = struct.unpack_from('<ii', payload, SCENE_WORD)
    lines = (run / 'stdout.log').read_text(errors='replace').splitlines()
    queued, activated = restore_field(lines)
    checks['field'] = {'pass': queued == activated == (scene, entrance),
                       'saved': [scene, entrance], 'queued': queued, 'activated': activated}
    record = xdb_scene((stage / 'FLDDATA' / 'FLDSET.XDB').read_bytes(), scene)
    stg_path = stage / 'FLDDATA' / record['room_directory'].upper() / f"{record['room'].upper()}.XPF" / 'a.stg'
    if not stg_path.is_file():
        stg_path = next(stage.glob(f"FLDDATA/**/{record['room'].upper()}.XPF/a.stg"))
    want_pos, want_rot = stg_entrance(stg_path.read_bytes(), entrance)
    actors = json.loads((run / 'control' / 'restored-field.png.actors-state.json').read_text())
    pos, rot = hero_pose(actors)
    checks['pose'] = {'pass': close(pos, want_pos) and close(rot, want_rot), 'room': record['room'],
                      'stg': str(stg_path), 'expected': {'position': want_pos, 'rotation': want_rot},
                      'restored': {'position': pos, 'rotation': rot}}
    if report_root is not None:
        loads = report_loads(report_root)
        same = [l for l in loads if l[2] == scene]
        restored = next((l for l in same if l[0] == 1), None)
        ordinary = [l for l in same if l[0] > 1]
        if restored is None or restored[3] != entrance or not ordinary:
            checks['bindings'] = {'pass': False, 'reason': 'report lacks the Restore load or a later ordinary load of the scene',
                                  'loads': [[c, s, e] for c, _, s, e in loads]}
        else:
            a, b = bindings(restored[1]), bindings(ordinary[0][1])
            diffs = compare_bindings(a, b)
            checks['bindings'] = {'pass': not diffs, 'restored_cycle': restored[0], 'ordinary_cycle': ordinary[0][0],
                                  'ordinary_entrance': ordinary[0][3], 'differences': diffs,
                                  'models': [m['name'] for m in a['models']], 'xta_images': len(a['xta_scene']),
                                  'handles': sum(len(m['handles']) for m in a['models']) + len(a['xta_scene']),
                                  'actors': a['actors']}
    return {'schema': 'okage.native-restore-audit.v1', 'run': str(run), 'save': str(save_file),
            'pass': all(c['pass'] for c in checks.values()), 'checks': checks}

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run')
    ap.add_argument('--stage', required=True, help='the run\'s pinned owned data stage')
    ap.add_argument('--save', required=True, help='the restored slot file (RUN/saves/<slot>)')
    ap.add_argument('--report', help='--newgame-report root of the Restore run')
    ap.add_argument('--json', help='write the result here (refuses to overwrite)')
    a = ap.parse_args(argv)
    try:
        r = audit(a.run, a.stage, a.save, a.report)
    except (Malformed, OSError, KeyError, ValueError, StopIteration) as e:
        print(json.dumps({'malformed': str(e)}))
        return 2
    text = json.dumps(r, indent=1)
    if a.json:
        out = Path(a.json)
        if out.exists():
            print(json.dumps({'malformed': f'{out} exists'}))
            return 2
        out.write_text(text + '\n')
    print(text)
    return 0 if r['pass'] else 1

if __name__ == '__main__':
    sys.exit(main())
