#!/usr/bin/env python3
"""Strict field-world component comparison against pinned raw retail observations.

pin ORIGINAL INVENTORY creates an explicit inventory; compare ORIGINAL NATIVE
--inventory INVENTORY [--json OUT] refuses any changed source input. ORIGINAL is
field-world-original-v1 with runs/world-fill00, runs/world-fillff and
vram-analysis-v1. Native world-route.json names every reached world checkpoint.
Both reserved queues retain their source identities; no selector normalization.

The ordered cold texture-resource operation substream is compared. Other node,
field, effect, UI and phase events are reported as excluded, never all-node parity.
Model bindings are checked only at path returns and map/worker/activation points;
zero-count raw material tables are disclosed as uncovered metadata. Full VRAM
nibble masks and defined values, every RNG draw and Session globals are compared.
No scheduling, image/audio, whole-raw-table or whole-game equality is claimed.
Exit: 0 component identical, 1 different, 2 malformed/unpinned, 4 unproven source
pair or asserted rows, 5 missing native coverage. Research only; never product input.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import retire_title_compare as base
import newgame_compare as h
import field_prefix_compare as i
import retire_title_original_extract as source_f
import newgame_original_extract as source_h
import field_prefix_extract as source_i

Malformed = base.Malformed
ELF = i.ELF
VRAM = 1 << 22
RAM = 1 << 25
SCENE_COUNT, SCENE_BASE = 0x337948, 0x337950
QUEUE_WRITE, QUEUE_READ, QUEUE_BASE = 0x1fe244, 0x1fe248, 0x31faf0
RESOURCE_KINDS = frozenset(base.EVENT_KEYS) - {'phase'} | {'upload-bytes'}
STATE_KEYS = base.STATE_KEYS + ('reserved1', 'reserve_class')
NATIVE_FILES = h.NATIVE_FILES + ('field.json', 'world.json')
FUNCTIONS = {'cache_enable_set': '0x186a50', 'xmd_load': '0x186aa8', 'xmd_load_path': '0x186c48',
             'xta_load': '0x1b6848', 'xta_image_load': '0x1b6710', 'field_map_load': '0x100b20',
             'field_worker': '0x132688', 'field_map_commit': '0x100c08', 'field_activate': '0x132720'}
ENDPOINTS = ('field-cache-enabled', 'field-map-loaded', 'field-worker-before-complete',
             'field-worker-return', 'field-map-committed', 'field-activation-return')
CALL_CP = {'cache_enable_set': 'field-cache-enabled', 'field_map_load': 'field-map-loaded',
           'field_worker': 'field-worker-return', 'field_map_commit': 'field-map-committed',
           'field_activate': 'field-activation-return'}
WORLD_CALLS = ('xmd_load', 'xmd_load_path', 'xta_image_load', 'xta_load')
PAIR_KEYS = frozenset(source_f.RESOURCE_STATE_KEYS) | {'vram.available', 'vram.cursors', 'vram.occupancy_sha256', 'vram.nodes_sha256'}
EXTRA_FIELD_OPS = {'cache-enable': {'value': int}, 'world-complete': {}, 'map-commit': {}, 'activated': {}}

def need(ok, message):
    if not ok:
        raise Malformed(message)

def obj(path):
    value = base.parse_json(base.read_text(path), str(path))
    need(isinstance(value, dict), f'{path}: expected object')
    return value

def rows(path):
    return [v for _, v in base.jsonl_rows(path)]

PARENT_ADDRESS_FUNCTIONS = {'mode_parent_tick': '0x124818', 'node_create': '0x1151b8',
    'mode_child_create': '0x1246b0', 'resource_retire': '0x101058',
    'successor_construct': '0x124548', 'manager_create': '0x11f5f8'}

def source_events(path):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                first = result[key]
                need(key == 'parent' and '_observed_caller_parent' not in result and isinstance(first, str)
                     and not first.startswith('0x') and isinstance(value, str) and value.startswith('0x')
                     and all(c in '0123456789abcdef' for c in value[2:]) and len(value) > 2,
                     'undocumented duplicate source JSON property')
                result['_observed_caller_parent'] = first
            result[key] = value
        return result
    result = []
    for line in base.read_text(path).splitlines():
        need(bool(line.strip()), 'blank source event row')
        event = json.loads(line, object_pairs_hook=pairs)
        if '_observed_caller_parent' in event:
            need(event.get('ev') == 'enter' and PARENT_ADDRESS_FUNCTIONS.get(event.get('name')) == event.get('fn'),
                 'duplicate source parent outside documented observer decoder')
        result.append(event)
    return result

def sha(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()

def checkpoints(meta):
    cps = meta.get('checkpoints')
    need(isinstance(cps, list) and all(isinstance(x, str) for x in cps) and len(cps) == len(set(cps)), 'source checkpoint list malformed')
    cps = [x for x in cps if x.startswith('world-') or x in ENDPOINTS]
    need(set(ENDPOINTS) <= set(cps), 'source world endpoints missing')
    need(cps[0] == ENDPOINTS[0] and cps[-1] == ENDPOINTS[-1], 'source world endpoints out of order')
    for family in WORLD_CALLS:
        group = [x for x in cps if x.startswith('world-' + family + '-')]
        need(group and group == [f'world-{family}-{n:04d}' for n in range(1, len(group) + 1)], f'source {family} checkpoint coverage missing/duplicated')
    return cps

def source_files(cps):
    result = ['runs/world-fill00/run-meta.json', 'runs/world-fill00/events.jsonl',
              'runs/world-fillff/run-meta.json', 'runs/world-fillff/events.jsonl', 'vram-analysis-v1/analysis.json']
    for cp in cps:
        for run in ('world-fill00', 'world-fillff'):
            result += [f'runs/{run}/checkpoints/{cp}/{f}' for f in ('state.jsonl', 'ee-ram-32mb.bin')]
        result += [f'vram-analysis-v1/{cp}/{f}' for f in ('analysis.json', 'vram-written-mask.bin', 'vram-written-values.bin', 'vram-invalidated-mask.bin')]
    return result

def pin(original, output):
    root = Path(original)
    cps = checkpoints(obj(root / 'runs/world-fill00/run-meta.json'))
    inventory = {'schema': 'okage.field-world-pins.v1', 'elf_sha256': ELF, 'checkpoints': cps,
                 'files': {rel: {'sha256': sha(root / rel), 'bytes': (root / rel).stat().st_size} for rel in source_files(cps)}}
    with open(output, 'x') as stream:
        json.dump(inventory, stream, indent=1)
        stream.write('\n')
    return inventory

def verify_inventory(root, path):
    inventory = obj(path)
    need(set(inventory) == {'schema', 'elf_sha256', 'checkpoints', 'files'} and inventory['schema'] == 'okage.field-world-pins.v1'
         and inventory['elf_sha256'] == ELF, 'inventory identity/schema mismatch')
    cps = checkpoints(obj(root / 'runs/world-fill00/run-meta.json'))
    need(inventory['checkpoints'] == cps and isinstance(inventory['files'], dict)
         and set(inventory['files']) == set(source_files(cps)), 'inventory lacks exact source checkpoint/file coverage')
    for rel, p in inventory['files'].items():
        need(isinstance(p, dict) and set(p) == {'sha256', 'bytes'} and i.hex_string(p['sha256'], 64)
             and base.is_int(p['bytes']) and p['bytes'] >= 0, f'invalid inventory pin {rel}')
        need((root / rel).stat().st_size == p['bytes'] and sha(root / rel) == p['sha256'], f'source pin mismatch {rel}')
    return cps

def verify_events(events, meta):
    need(len(events) == meta.get('events') and all(isinstance(e, dict) and e.get('i') == n for n, e in enumerate(events)),
         'source event stream missing/duplicated/truncated indices')
    need(meta.get('field_world') is True and meta.get('world_stopped') is True and meta.get('missing_checkpoints') == []
         and meta.get('rand_pending_at_stop') is False, 'source run lacks completed world/every-draw coverage')
    need(events[-1].get('ev') == 'stop' and events[-1].get('kind') == 'field_world_complete'
         and events[-1].get('pc') == '0x1344b4', 'source activation stop missing')
    entered, exited = {}, set()
    for e in events:
        if e['ev'] == 'enter':
            entered[e['i']] = e
            if e.get('name') in FUNCTIONS:
                need(e.get('fn') == FUNCTIONS[e['name']], f'source function identity mismatch {e["name"]}')
        elif e['ev'] == 'exit':
            ent = entered.get(e.get('enter'))
            need(ent is not None and e['enter'] not in exited and e.get('name') == ent.get('name') and e.get('fn') == ent.get('fn'),
                 f'source unmatched/duplicate return at {e["i"]}')
            exited.add(e['enter'])
    open_names = [e['name'] for at, e in sorted(entered.items()) if at not in exited]
    need(meta.get('open_frames_at_stop') == open_names, 'source truncated/unreturned hooked function coverage')
    return entered

def source_boundaries(events, cps):
    selected = {e['name']: e for e in events if e['ev'] == 'checkpoint' and e['name'] in cps}
    need(list(selected) == cps, 'source checkpoint events disagree with run metadata')
    counts = collections.Counter()
    expected = []
    armed = False
    for e in events:
        if e['ev'] == 'exit' and e.get('name') == 'cache_enable_set' and not armed:
            # Only the world cache-enable call (immediately followed by its named marker).
            if e['i'] + 1 < len(events) and events[e['i'] + 1].get('checkpoint') == 'field-cache-enabled':
                armed = True
        if not armed or e['ev'] != 'exit':
            continue
        name = e.get('name')
        if name in WORLD_CALLS:
            counts[name] += 1
            cp = f'world-{name}-{counts[name]:04d}'
        elif name in CALL_CP:
            cp = CALL_CP[name]
        else:
            continue
        marker = events[e['i'] + 1]
        need(marker.get('ev') == 'field_world_boundary' and marker.get('checkpoint') == cp
             and marker.get('fn') == FUNCTIONS[name] and marker.get('actual_pc') == marker.get('return_pc'),
             f'source {name} return lacks exact boundary witness')
        need(cp in selected and selected[cp]['i'] == marker['i'] + 1 and selected[cp]['pc'] == marker['actual_pc'],
             f'source {cp} snapshot not at witnessed return')
        expected.append(cp)
    need([x for x in cps if x != 'field-worker-before-complete'] == expected, 'source reached-call checkpoint omission')
    before = selected['field-worker-before-complete']
    need(before['pc'] == '0x1326c0' and events[before['i'] - 1].get('name') == 'field_prefix'
         and events[before['i'] - 1].get('ev') == 'exit', 'source worker completion pre-store boundary missing')
    return {cp: selected[cp]['i'] for cp in cps}

def resource_stream(events):
    try:
        lifecycle, details, _ = source_h.timeline(events)
    except (ValueError, KeyError) as exc:
        raise Malformed(f'source lifecycle extraction: {exc}') from exc
    world_i = next(e['i'] for e in events if e['ev'] == 'checkpoint' and e['name'] == 'field-cache-enabled')
    result = []
    for row, detail in zip(lifecycle, details):
        if row['kind'] in RESOURCE_KINDS:
            row = dict(row)
            if row['kind'] == 'material' and detail['oracle_i'] >= world_i:
                row['model'] = row['model'].split('/')[-1]
            result.append((detail['oracle_i'], row))
    return result

def source_rng(events):
    result, pending, seed = [], None, 1
    for e in events:
        if e['ev'] == 'rand':
            need(pending is None and e.get('n') == len(result) and e.get('state') == seed, 'source RNG entry/index/chain coverage')
            pending = e
        elif e['ev'] == 'rand_exit':
            need(pending is not None and e.get('n') == pending['n'], 'source RNG return without entry')
            seed = (seed * 0x41c64e6d + 0x3039) & 0xffffffff
            need(e.get('state') == seed and int(e['v0']) & 0xffffffff == seed & 0x7fffffff, 'source RNG value/LCG mismatch')
            frames = pending.get('frames', [])
            phases = [phase for frame, phase in (('noise_texture', 'noise'), ('effect_noise_grid', 'effect-grid'), ('efd_import', 'effect-template')) if frame in frames]
            need(len(phases) <= 1, 'ambiguous source RNG consumer')
            phase = phases[0] if phases else source_h.RNG_SITES.get(pending.get('ra'), 'session')
            result.append((pending['i'], {'i': len(result), 'phase': phase, 'state': seed, 'value': seed & 0x7fffffff}))
            pending = None
    need(pending is None, 'truncated source RNG return')
    return result

def verify_resource_history(prefix, raw):
    allocations, handles = {}, {}
    for row in prefix:
        kind = row['kind']
        if kind == 'alloc' and row['alloc'] >= 0:
            need(row['alloc'] not in allocations, 'source allocation event duplicated')
            allocations[row['alloc']] = [True, 0]
        elif kind == 'ref':
            need(row['alloc'] in allocations, 'source ref lacks allocation event')
            state = allocations[row['alloc']]
            need(state[0] and state[1] + row['delta'] == row['refs'], 'source ref event omission/chain inconsistency')
            state[1] = row['refs']
        elif kind == 'free':
            need(row['alloc'] in allocations and allocations[row['alloc']][0], 'source free without live allocation')
            allocations[row['alloc']][0] = False
        elif kind == 'handle' and row['handle'] >= 0:
            need(row['handle'] not in handles, 'source duplicate live handle creation')
            handles[row['handle']] = row['record']
        elif kind == 'retire':
            need(row['handle'] in handles, 'source retirement without observed handle')
            handles.pop(row['handle'])
    need(set(allocations) == {a['id'] for a in raw['allocations']}, 'source allocation event coverage disagrees with snapshot')
    for allocation in raw['allocations']:
        need(allocations[allocation['id']][0] == allocation['live'], 'source allocation lifetime differs from snapshot')
        if allocation['live']:
            need(allocations[allocation['id']][1] == allocation['refs'], 'source reference chain differs from snapshot')
    need(sorted(handles.items()) == [tuple(x) for x in raw['handles']], 'source handle event coverage disagrees with snapshot')

def convert_state(raw, cp):
    serial = dict(raw['allocation_serials'])
    allocations = []
    for a in raw['allocations']:
        if a['live']:
            d = a['desc']
            allocations.append(dict(id=a['id'], serial=serial[a['id']], **{'class': a['class']}, psm=d['psm'],
                width=d['w'], height=d['h'], tbp=d['tbp'], tbw=d['tbw'], references=a['refs'], descriptor=int(d['raw'], 16)))
    allocations.sort(key=lambda a: a['id'])
    live = {a['id'] for a in allocations}
    def ref(n):
        need(n < 0 or n in live, 'source record refers to dead allocation')
        return None if n < 0 else [n, serial[n]]
    recs = {r['r']: r for r in raw['records']}
    handles = [dict(handle=n, record=r, image=ref(recs[r]['image']), palettes=[ref(recs[r]['pal0'])] + [ref(x) for x in recs[r]['extra']])
               for n, r in raw['handles']]
    return {'coverage': {'lifecycle': 'field-world', 'checkpoint': cp, 'memory': base.MEMORY},
        'allocations': allocations, 'handles': handles,
        'records': [dict(record=r['r'], name=r['name'], hash_slot=r['hash_slot'], handles=r['handles']) for r in sorted(raw['records'], key=lambda r: r['r'])],
        'hash': raw['hash'], 'reserved': raw['reserved'], 'reserved1': raw['reserved1'], 'reserve_class': raw['reserve_class'],
        'free_records': raw['free_records'], 'free_extras': raw['free_extras'], 'available': raw['vram']['available'],
        'cursors': raw['vram']['cursors'], 'free_queue_ids': raw['free_queue_ids'], 'occupancy_hex': raw['occupancy_hex'],
        'image_list': raw['image_list'], 'materials': []}

def validate_state(value, cp):
    need(set(STATE_KEYS) <= set(value) and not set(value) - set(STATE_KEYS) - set(base.IGNORED_STATE_KEYS),
         f'native {cp} state keys/coverage missing or unknown')
    need(value['coverage'] == {'lifecycle': 'field-world', 'checkpoint': cp, 'memory': base.MEMORY}, 'native state coverage label mismatch')
    need(value['reserve_class'] in (0, 1) and base.is_int(value['reserve_class']), 'invalid reserve selector')
    for k in ('reserved', 'reserved1', 'cursors', 'free_queue_ids', 'image_list'):
        base._ints(value[k], k, 64 if k == 'cursors' else None)
    need(i.hex_string(value['occupancy_hex'], 16384), 'state occupancy malformed')
    for k in ('allocations', 'handles', 'records', 'hash', 'materials'):
        need(isinstance(value[k], list), f'state {k} not a list')
    for a in value['allocations']:
        need(isinstance(a, dict) and set(a) == set(base.ALLOC_KEYS) and all(base.is_int(x) for x in a.values()), 'state allocation malformed')
    base._ascending([a['id'] for a in value['allocations']], 'allocations')
    for handle in value['handles']:
        need(isinstance(handle, dict) and set(handle) == {'handle', 'record', 'image', 'palettes'}
             and base.is_int(handle['handle']) and base.is_int(handle['record'])
             and isinstance(handle['palettes'], list) and handle['palettes'], 'state handle malformed')
        for ref in [handle['image']] + handle['palettes']:
            base._pair(ref, 'handle allocation reference')
    base._ascending([a['handle'] for a in value['handles']], 'handles')
    for record in value['records']:
        need(isinstance(record, dict) and set(record) == {'record', 'name', 'hash_slot', 'handles'}
             and isinstance(record['name'], str) and all(base.is_int(record[k]) for k in ('record', 'hash_slot', 'handles')), 'state record malformed')
    base._ascending([a['record'] for a in value['records']], 'records')
    for ref in value['hash']:
        base._pair(ref, 'hash reference', nullable=False)
    base._ascending([a[0] for a in value['hash']], 'hash slots')
    for k in ('free_records', 'free_extras', 'available'):
        need(base.is_int(value[k]), f'state {k} not integer')
    for slot, ref in enumerate(value['materials']):
        base._pair(ref, 'material slot', nullable=False)
        need(ref[0] == slot, 'state material slots must be contiguous')
    return {key: value[key] for key in STATE_KEYS}

def native_events(path):
    resource, excluded, asserted = [], collections.Counter(), 0
    specs, ops = dict(i.EVENT_KEYS), {k: dict(v) for k, v in i.OP_KEYS.items()}
    ops['field'] = {**ops['field'], **EXTRA_FIELD_OPS}
    for number, row in base.jsonl_rows(path):
        need(isinstance(row, dict), f'{path}:{number}: invalid lifecycle object')
        row, owner = i.g._asserted(row, f'{path}:{number}')
        kind = row.get('kind')
        need(kind in specs or kind in ops, f'{path}:{number}: unknown lifecycle kind')
        if kind in ops:
            need(row.get('op') in ops[kind], f'{path}:{number}: unknown operation')
            spec, fixed = ops[kind][row['op']], {'kind', 'op'}
        else:
            spec, fixed = specs[kind], {'kind'}
        need(set(row) == set(spec) | fixed and all(i._typed(row[k], t) for k, t in spec.items()), f'{path}:{number}: lifecycle shape/type mismatch')
        if kind in RESOURCE_KINDS:
            resource.append(row)
            asserted += owner
        else:
            excluded[kind + ':' + str(row.get('op', ''))] += 1
    return resource, dict(excluded), asserted

def scene_from_ram(path):
    need(path.stat().st_size == RAM, f'{path}: source RAM extent')
    with open(path, 'rb') as stream:
        stream.seek(SCENE_COUNT)
        count = struct.unpack('<I', stream.read(4))[0]
        need(count <= (RAM - SCENE_BASE) // 4, 'XTA scene array out of RAM range')
        stream.seek(SCENE_BASE)
        return list(struct.unpack('<' + 'i' * count, stream.read(count * 4)))

def queued_field_job(path, raw):
    with open(path, 'rb') as stream:
        def word(address):
            need(0 <= address <= RAM - 4, 'source job queue word outside RAM')
            stream.seek(address)
            return struct.unpack('<I', stream.read(4))[0]
        write, read = word(QUEUE_WRITE), word(QUEUE_READ)
        need(write < 32 and read < 32, 'source job ring indices malformed')
        manager = raw['field']['manager']
        manager = int(manager, 16) if isinstance(manager, str) else manager
        found = 0
        for delta in range((write - read) & 31):
            at = QUEUE_BASE + ((read + delta) & 31) * 16
            if word(at) == 0x132688 and word(at + 4) == manager:
                found += 1
        need(found <= 1, 'source field worker queued twice')
        return bool(found)

def inherited_effect106(raw, events, boundary):
    current = source_f.effect_state(raw['title_effect']) if raw['title_effect_slot'] >= 0 else None
    if current is None or current['shape'] in (None, 106):
        return current, None
    entered = {e['i']: e for e in events if e['ev'] == 'enter'}
    slot = raw['title_effect_slot']
    original = None
    reclaimed = False
    reused = False
    for event in events:
        if event['i'] >= boundary:
            break
        if event['ev'] == 'exit' and event.get('name') == 'effect_alloc' and event.get('slot') == slot:
            shape = entered[event['enter']]['shape']
            if shape == 106:
                need(original is None, 'ambiguous second title effect106 lifetime')
                original = event['i']
            elif original is not None:
                need(reclaimed, 'effect slot reused before original effect106 reclaim')
                reused = True
        elif event['ev'] == 'enter' and event.get('name') == 'effect_free' and event.get('job_slot') == slot and original is not None:
            reclaimed = True
    need(original is not None and reclaimed and reused, 'source effect106 slot reuse lacks lifetime witness')
    return {'slot': slot, 'shape': None, 'state': 'reclaimed', 'element_handles': []}, current

def source_world(raw, events, boundary, ram_path):
    archive, model_slots = None, {}
    entered = {e['i']: e for e in events if e['ev'] == 'enter'}
    for e in events:
        if e['i'] >= boundary:
            break
        if e['ev'] == 'enter' and e['name'] == 'archive_mount':
            archive = e.get('str')
        if e['ev'] == 'exit' and e['name'] == 'xmd_load_path' and e.get('result', -1) >= 0:
            ent = entered[e['enter']]
            model_slots[e['result']] = ent['str']
    tables = raw['material_tables']
    need(isinstance(tables, list) and all(isinstance(t, dict) and set(t) == {'ptr', 'next', 'word1', 'word2', 'count', 'handles'}
         and base.is_int(t['word1']) and base.is_int(t['count']) and t['count'] == len(t['handles']) for t in tables), 'source material table coverage/schema')
    nonempty = sorted((t for t in tables if t['count']), key=lambda t: t['word1'])
    need(len({t['word1'] for t in nonempty}) == len(nonempty), 'source model slots duplicated')
    models = []
    for t in nonempty:
        need(t['word1'] in model_slots, 'source material binding without observed path return')
        models.append({'name': model_slots[t['word1']], 'handles': t['handles']})
    scene = scene_from_ram(ram_path)
    observed = [int(e['v0'], 16) if int(e['v0'], 16) < 0x80000000 else int(e['v0'], 16) - 0x100000000
                for e in events if e['i'] < boundary and e['ev'] == 'exit' and e['name'] == 'xta_image_load']
    need(scene == observed, 'source XTA scene RAM list does not match reached image returns')
    return {'models': models, 'xta_scene': scene}, [t for t in tables if not t['count']]

def compare(original, native, inventory, vram_size=VRAM):
    root, native = Path(original), Path(native)
    need(root.resolve() != native.resolve() and root.resolve() not in native.resolve().parents and native.resolve() not in root.resolve().parents, 'self/nested comparison refused')
    cps = verify_inventory(root, inventory)
    pin_data = obj(inventory)['files']
    source_inodes = {(p.stat().st_dev, p.stat().st_ino) for p in (root / rel for rel in source_files(cps))}
    for cp in cps:
        for name in NATIVE_FILES:
            p = native / cp / name
            if p.exists():
                need((p.stat().st_dev, p.stat().st_ino) not in source_inodes, 'shared source/native inode refused')
    meta = obj(root / 'runs/world-fill00/run-meta.json')
    events = source_events(root / 'runs/world-fill00/events.jsonl')
    verify_events(events, meta)
    bounds = source_boundaries(events, cps)
    stream = resource_stream(events)
    rng = source_rng(events)
    ff_meta = obj(root / 'runs/world-fillff/run-meta.json')
    ff_events = source_events(root / 'runs/world-fillff/events.jsonl')
    need(checkpoints(ff_meta) == cps, 'paired source reached checkpoints differ')
    verify_events(ff_events, ff_meta)
    ff_bounds = source_boundaries(ff_events, cps)
    ff_stream, ff_rng = resource_stream(ff_events), source_rng(ff_events)
    need(meta.get('rand_calls') == len(rng), 'source RNG total metadata disagrees')
    paired = obj(root / 'vram-analysis-v1/analysis.json')
    need(set(paired.get('checkpoints', {})) == set(cps) and paired.get('pair') == ['world-fill00', 'world-fillff']
         and isinstance(paired.get('failures'), list), 'paired source checkpoint evidence missing')
    result = {'schema': 'okage.field-world-comparison.v1', 'checkpoints': {}, 'missing': [],
        'raw_duplicate_parent_rows': sum('_observed_caller_parent' in e for e in events),
        'source_failures': paired['failures'], 'whole_timeline_parity': False, 'whole_raw_table_parity': False,
        'limitations': ['Cold texture-resource operation substream only; excluded node/effect/UI/field rows are reported.',
                        'Model arrays only at source path/map/worker/activation boundaries; zero-count tables uncovered.',
                        'Scheduling and media parity unclaimed. Raw source field counters not represented by field.json are unclaimed.']}
    missing = [f'{cp}/{name}' for cp in cps for name in NATIVE_FILES if not (native / cp / name).is_file()]
    if not (native / 'world-route.json').exists():
        missing.append('world-route.json')
    if missing:
        return 5, dict(result, result='INCOMPLETE', missing=missing)
    route = obj(native / 'world-route.json')
    need(set(route) == {'route', 'elf_sha256', 'checkpoints'} and route['route'] == 'newgame-field-world' and route['elf_sha256'] == ELF
         and set(route['checkpoints']) == set(cps) and all(base.is_int(x) and x >= 0 for x in route['checkpoints'].values()), 'native route identity/checkpoint coverage')
    need([route['checkpoints'][cp] for cp in cps] == sorted(route['checkpoints'].values()), 'native checkpoint order')
    result['scheduler_native'] = route
    result['scheduler_original'] = {cp: {'vb': events[bounds[cp]]['vb'], 'pc': events[bounds[cp]]['pc']} for cp in cps}
    bad, asserted, previous = False, 0, None
    field_rows = [r for _, r in source_i.field_rows(events) if r['op'] != 'env-bind']
    for cp in cps:
        od = root / 'runs/world-fill00/checkpoints' / cp
        raw_rows = rows(od / 'state.jsonl')
        need(len(raw_rows) == 1, f'source {cp} state endpoint duplicate/missing')
        raw = raw_rows[0]
        other_rows = rows(root / 'runs/world-fillff/checkpoints' / cp / 'state.jsonl')
        need(len(other_rows) == 1, 'paired source state endpoint duplicate/missing')
        required_raw = set(source_f.RESOURCE_STATE_KEYS) | {'vram', 'game', 'mode_parent_fields', 'field', 'title_effect_slot', 'title_effect'}
        for snapshot in (raw, other_rows[0]):
            need(required_raw <= set(snapshot), 'source raw state property coverage missing')
            need(set(snapshot['vram']) >= {'available', 'cursors', 'occupancy_sha256', 'nodes_sha256'}, 'source raw VRAM state coverage missing')
            need(set(snapshot['field']) == i.CONTROL_FIELD_KEYS | {'w38'}, 'source raw field property coverage missing')
        direct_pair = source_f.resource_equal(raw, other_rows[0])
        need(set(direct_pair) == PAIR_KEYS, 'source pair witness property coverage')
        proof = paired['checkpoints'][cp]
        need(obj(root / 'vram-analysis-v1' / cp / 'analysis.json') == proof, 'source per-checkpoint paired proof differs from aggregate')
        ee_files = [f'runs/{run}/checkpoints/{cp}/ee-ram-32mb.bin' for run in ('world-fill00', 'world-fillff')]
        need(proof.get('raw_ee_ram_equal') is True and pin_data[ee_files[0]]['sha256'] == pin_data[ee_files[1]]['sha256']
             and proof.get('writer_ledger_equal') is True and proof.get('ever_upload_ledger_equal') is True, 'source paired RAM/GS ledger witness failing/missing')
        need(set(proof.get('resource_state_equal', {})) == PAIR_KEYS
             and proof['resource_state_equal'] == direct_pair, 'paired source property witness does not describe actual raw states')
        need(all(direct_pair.values()) and proof.get('rng_state_equal') is True and raw['rand_state_214ec8_58'] == other_rows[0]['rand_state_214ec8_58'], 'source paired resource/RNG state differs')
        boundary = bounds[cp]
        oe = [r for at, r in stream if at < boundary]
        need(oe == [r for at, r in ff_stream if at < ff_bounds[cp]], 'paired source ordered resource operations differ')
        verify_resource_history(oe, raw)
        ne, excluded, nas = native_events(native / cp / 'lifecycle.jsonl')
        org = [r for at, r in rng if at < boundary]
        need(org == [r for at, r in ff_rng if at < ff_bounds[cp]], 'paired source observed RNG operations differ')
        nrg, nra = h.load_rng(native / cp / 'rng.jsonl', native=True)
        need(org and org[-1]['state'] == raw['rand_state_214ec8_58'], 'source snapshot RNG state not equal to complete observed chain')
        os_ = convert_state(raw, cp)
        ns = validate_state(obj(native / cp / 'state.json'), cp)
        effect106, reused_slot = inherited_effect106(raw, events, boundary)
        session = source_h.session_state(raw)
        field = source_i.field_json(raw['field'], field_rows)
        field['job_queued'] = queued_field_job(od / 'ee-ram-32mb.bin', raw)
        nf, gaps = i.load_field(native / cp / 'field.json', native=True)
        need(not gaps, f'native {cp} field coverage missing {gaps}')
        nw = obj(native / cp / 'world.json')
        need(set(nw) == {'models', 'xta_scene'} and isinstance(nw['models'], list) and isinstance(nw['xta_scene'], list), 'native world schema')
        for model in nw['models']:
            need(isinstance(model, dict) and set(model) == {'name', 'handles'} and isinstance(model['name'], str), 'native world model schema')
            base._ints(model['handles'], 'world model handles')
        base._ints(nw['xta_scene'], 'XTA scene handles')
        bindings = 'xmd_load_path' in cp or cp in ENDPOINTS[1:]
        if bindings:
            world, zero = source_world(raw, events, boundary, od / 'ee-ram-32mb.bin')
        else:
            world, zero = {'xta_scene': scene_from_ram(od / 'ee-ram-32mb.bin')}, [t for t in raw['material_tables'] if t['count'] == 0]
        vd = root / 'vram-analysis-v1' / cp
        v = i.compare_vram(vd, native / cp, vram_size)
        for filename, key in (('vram-written-mask.bin', 'mask_sha256'), ('vram-written-values.bin', 'values_sha256'), ('vram-invalidated-mask.bin', 'invalidated_sha256')):
            need(sha(vd / filename) == proof.get('vram', {}).get(key), 'source VRAM mask/value proof digest mismatch')
        differences = {'lifecycle': i.first_difference(oe, ne), 'rng': i.first_difference(org, nrg),
            'state': {k: i.first_difference(os_[k], ns[k]) for k in STATE_KEYS if os_[k] != ns[k]},
            'effects': i.first_difference(effect106, base.load_effects(native / cp / 'effects.json')),
            'session': i.first_difference(session, h.load_session(native / cp / 'session.json')),
            'field': i.first_difference(field, nf), 'world': {k: i.first_difference(v, nw[k]) for k, v in world.items() if v != nw[k]}}
        inconsistent = previous is not None and (ne[:len(previous[0])] != previous[0] or nrg[:len(previous[1])] != previous[1])
        previous = ne, nrg
        cp_bad = any(differences.values()) or any(v[k] for k in ('known_mask_mismatch', 'invalidated_mask_mismatch', 'value_mismatch')) or inconsistent
        bad |= cp_bad
        asserted += nas + sum(nra)
        result['checkpoints'][cp] = {'differences': differences, 'vram': v, 'stream_inconsistent': inconsistent,
            'excluded_native_rows': excluded, 'resource_events_original': len(oe), 'resource_events_native': len(ne),
            'rng_draws_original': len(org), 'rng_draws_native': len(nrg), 'models_compared': bindings,
            'excluded_native_state_metadata': sorted(set(obj(native / cp / 'state.json')) & set(base.IGNORED_STATE_KEYS)),
            'uncovered_zero_count_tables': zero, 'uncovered_reused_effect_slot': reused_slot, 'asserted_rows': nas + sum(nra), 'identical': not cp_bad}
    code, label = (1, 'DIFFERENT') if bad else (4, 'UNPROVEN') if asserted or paired['failures'] else (0, 'COMPONENT-IDENTICAL')
    return code, dict(result, result=label, payload_identical=not bad, asserted_rows=asserted)

def run(*args, **kwargs):
    try:
        return compare(*args, **kwargs)
    except (Malformed, OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        return 2, {'schema': 'okage.field-world-comparison.v1', 'result': 'MALFORMED', 'error': str(exc)}

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    pins = sub.add_parser('pin')
    pins.add_argument('original')
    pins.add_argument('inventory')
    compare_parser = sub.add_parser('compare')
    compare_parser.add_argument('original')
    compare_parser.add_argument('native')
    compare_parser.add_argument('--inventory', required=True)
    compare_parser.add_argument('--json')
    args = parser.parse_args(argv)
    if args.command == 'pin':
        pin(args.original, args.inventory)
        return 0
    code, result = run(args.original, args.native, args.inventory)
    if args.json:
        with open(args.json, 'x') as stream:
            json.dump(result, stream, indent=1)
            stream.write('\n')
    print(json.dumps(result, indent=1))
    return code

if __name__ == '__main__':
    sys.exit(main())
