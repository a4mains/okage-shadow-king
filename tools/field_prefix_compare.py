#!/usr/bin/env python3
"""Strict comparison of pinned Field original field-prefix checkpoint payloads.

Usage: field_prefix_compare.py ORIGINAL NATIVE --inventory PINS [--json OUT]
PINS is an explicit JSON SHA inventory, schema okage.field-prefix-pins.v1,
with elf_sha256, checkpoints (the seven names in order), and files mapping each
relative original filename to {sha256, bytes}. Required pins are reference.json
and all ORIGINAL_FILES at every checkpoint. Inventories are research provenance,
not a claim that the reference capture independently proves the rest of the game.

Only lifecycle phase markers and documented state/effect diagnostic metadata are
excluded. Every other lifecycle row remains ordered; all allocation/handle state,
RNG draws, effect106, session and field words are compared. GS definedness and
invalidation masks must match exactly, with values compared on defined nibbles.
Timing is a separate gate and never excuses payload differences. Exit codes:
0 component payload identical (scheduling separately retained); 1 payload different;
2 malformed/unpinned input; 4 asserted-only/unproven reference evidence;
5 missing native coverage. Missing reserve_class_3abb14 is explicitly incomplete.
No comparator globals in reused modules are changed. Research tool only.
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import retire_title_compare as base
import restore_compare as g
import newgame_compare as h

CHECKPOINTS = ('field-constructor-return', 'worker-entry', 'atlas-loaded', 'texenv-bound',
               'texenv-loaded', 'ruw-loaded', 'ui-prefix-published')
ORIGINAL_FILES = h.ORIGINAL_FILES + ('field.json',)
NATIVE_FILES = h.NATIVE_FILES + ('field.json',)
VRAM = 1 << 22
MEMORY = base.MEMORY
Malformed = base.Malformed
parse_json = base.parse_json
read_text = base.read_text
is_int = base.is_int
canon = base.canon
jsonl_rows = base.jsonl_rows
STATE_KEYS = base.STATE_KEYS
IGNORED_STATE_KEYS = base.IGNORED_STATE_KEYS
ALLOC_KEYS = base.ALLOC_KEYS
_ints = base._ints
_pair = base._pair
_ascending = base._ascending
EVENT_KEYS = dict(h.EVENT_KEYS)
OP_KEYS = dict(h.OP_KEYS)
OP_KEYS['field'] = {
    'switch-337e70': {'value': int},
    'job-enqueue': {'job': ('0x132688',), 'scene': int, 'arg': int},
    'worker-entry': {}, 'clut-class': {'value': int}, 'lower-case': {'value': int},
    'resource-create': {}, 'atlas': {'handle': int}, 'texenv': {'handle': int},
    'env-bind': {'handle': int, 'tex0': 'hex64', 'region': 'int4'},
    'ruw': {'slot': int, 'bytes': int, 'sha256': 'sha256'}, 'ui-available': {},
}
FIELD_BOOL = ('resource_created', 'job_queued', 'ruw_slot0_live')
FIELD_INT = ('switch_337e70', 'scene', 'entrance', 'w30', 'w34', 'manager_60', 'resource_48',
             'atlas_global_24f730', 'clut_class_2138c4', 'lower_1fe30c', 'cache_1fe310',
             'ruw_bytes', 'reserve_class_3abb14')
FIELD_KEYS = FIELD_BOOL + FIELD_INT + ('tex0_332430', 'region_332450', 'ruw_sha256')

def hex_string(v, length):
    return isinstance(v, str) and len(v) == length and all(c in '0123456789abcdef' for c in v)

def _typed(v, t):
    if t == 'hex64':
        return isinstance(v, str) and v.startswith('0x') and hex_string(v[2:], 16)
    if t == 'sha256':
        return hex_string(v, 64)
    if t == 'int4':
        return isinstance(v, list) and len(v) == 4 and all(is_int(x) and 0 <= x <= 0xffffffff for x in v)
    return h._typed(v, t)

def load_lifecycle(path, native=False):
    rows, asserted = [], []
    for n, e in jsonl_rows(path):
        if not isinstance(e, dict):
            raise Malformed(f'{path}:{n}: not an object')
        a = False
        if native:
            e, a = g._asserted(e, f'{path}:{n}')
        elif 'asserted' in e:
            raise Malformed(f'{path}:{n}: original rows are observations and never carry "asserted"')
        if e.get('kind') not in set(EVENT_KEYS) | set(OP_KEYS):
            raise Malformed(f'{path}:{n}: unknown event kind {e.get("kind")!r}')
        if e['kind'] in OP_KEYS:
            ops = OP_KEYS[e['kind']]
            if e.get('op') not in ops:
                raise Malformed(f'{path}:{n}: unknown {e["kind"]} op {e.get("op")!r}')
            spec, fixed = ops[e['op']], {'kind', 'op'}
        else:
            spec, fixed = EVENT_KEYS[e['kind']], {'kind'}
        if set(e) != set(spec) | fixed:
            raise Malformed(f'{path}:{n}: {e["kind"]} keys {sorted(e)} != {sorted(set(spec) | fixed)}')
        for k, t in spec.items():
            if not _typed(e[k], t):
                raise Malformed(f'{path}:{n}: {e["kind"]}.{k} = {e[k]!r} has the wrong type/value')
        if e['kind'] == 'upload-bytes' and (len(e['sha256']) != 64 or any(c not in '0123456789abcdef' for c in e['sha256'])):
            raise Malformed(f'{path}:{n}: upload-bytes sha256 is not 64 lower-case hex digits')
        rows.append(e)
        asserted.append(a)
    if not any(r['kind'] != 'phase' for r in rows):
        raise Malformed(f'{path}: no lifecycle events')
    return rows, asserted

def load_state(path, cp):
    s = parse_json(read_text(path), str(path))
    if not isinstance(s, dict):
        raise Malformed(f'{path}: not an object')
    missing = set(STATE_KEYS) - set(s)
    unknown = set(s) - set(STATE_KEYS) - set(IGNORED_STATE_KEYS)
    if missing or unknown:
        raise Malformed(f'{path}: missing keys {sorted(missing)}, unknown keys {sorted(unknown)}')
    cov = {'lifecycle': 'field-prefix', 'checkpoint': cp, 'memory': MEMORY}
    if s['coverage'] != cov:
        raise Malformed(f'{path}: coverage {s["coverage"]!r} != {cov!r}')
    if not isinstance(s['allocations'], list):
        raise Malformed('allocations is not a list')
    for a in s['allocations']:
        if not isinstance(a, dict) or set(a) != set(ALLOC_KEYS) or not all(is_int(a[k]) for k in ALLOC_KEYS):
            raise Malformed(f'{path}: allocation entry {a!r} has the wrong keys/types')
    _ascending([a['id'] for a in s['allocations']], 'allocations')
    if not isinstance(s['handles'], list):
        raise Malformed('handles is not a list')
    for h in s['handles']:
        if not isinstance(h, dict) or set(h) != {'handle', 'record', 'image', 'palettes'} or not is_int(h['handle']) \
                or not is_int(h['record']) or not isinstance(h['palettes'], list) or not h['palettes']:
            raise Malformed(f'{path}: handle entry {h!r} has the wrong keys/types')
        _pair(h['image'], 'handle image')
        for p in h['palettes']:
            _pair(p, 'handle palette')
    _ascending([h['handle'] for h in s['handles']], 'handles')
    if not isinstance(s['records'], list):
        raise Malformed('records is not a list')
    for r in s['records']:
        if not isinstance(r, dict) or set(r) != {'record', 'name', 'hash_slot', 'handles'} or not is_int(r['record']) \
                or type(r['name']) is not str or not is_int(r['hash_slot']) or not is_int(r['handles']):
            raise Malformed(f'{path}: record entry {r!r} has the wrong keys/types')
    _ascending([r['record'] for r in s['records']], 'records')
    if not isinstance(s['hash'], list):
        raise Malformed('hash is not a list')
    for p in s['hash']:
        _pair(p, 'hash', nullable=False)
    _ascending([p[0] for p in s['hash']], 'hash slots')
    _ints(s['reserved'], 'reserved')
    _ints(s['cursors'], 'cursors', 64)
    _ints(s['free_queue_ids'], 'free_queue_ids')
    _ints(s['image_list'], 'image_list')
    for k in ('free_records', 'free_extras', 'available'):
        if not is_int(s[k]):
            raise Malformed(f'{k} is not an integer')
    o = s['occupancy_hex']
    if type(o) is not str or len(o) != 16384 or any(c not in '0123456789abcdef' for c in o):
        raise Malformed('occupancy_hex must be 16384 lower-case hex digits (8192 bytes)')
    if not isinstance(s['materials'], list):
        raise Malformed('materials is not a list')
    for i, p in enumerate(s['materials']):
        _pair(p, 'materials', nullable=False)
        if p[0] != i:
            raise Malformed('materials slots must be 0..n-1 in order')
    return s

ROUTE = 'newgame-field-prefix'
ELF = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
EVENT_COUNTS = dict(zip(CHECKPOINTS, (1271, 1271, 1285, 1297, 1297, 1298, 1299)))
RNG_COUNT = 11848
FIELD_ORDER = ('resource-create', 'switch-337e70', 'job-enqueue', 'worker-entry', 'clut-class',
               'lower-case', 'atlas', 'texenv', 'env-bind', 'ruw', 'ui-available')
FIELD_COUNTS = dict(zip(CHECKPOINTS, (3, 3, 7, 9, 9, 10, 11)))

def coverage(cp, events, rng, field):
    problems = []
    rows = [e for e in events if e['kind'] != 'phase']
    if len(rows) != EVENT_COUNTS[cp]:
        problems.append(f'lifecycle count {len(rows)} != frozen cold-route count {EVENT_COUNTS[cp]}')
    if [e['op'] for e in rows if e['kind'] == 'field'] != list(FIELD_ORDER[:FIELD_COUNTS[cp]]):
        problems.append('field operation frontier missing, duplicated, out of order or beyond endpoint')
    seed = 1
    for i, row in enumerate(rng):
        seed = (seed * 1103515245 + 12345) & 0xffffffff
        if row['state'] != seed or row['value'] != seed & 0x7fffffff:
            problems.append(f'RNG LCG chain/value invalid at draw {i}')
            break
    if len(rng) != RNG_COUNT:
        problems.append(f'RNG coverage {len(rng)} != inherited cold-route draws {RNG_COUNT}')
    expected = {'resource_created': True, 'job_queued': True, 'switch_337e70': 1, 'scene': 72, 'entrance': 0,
                'w30': 0, 'w34': int(cp == 'ui-prefix-published'), 'cache_1fe310': 0}
    for k, v in expected.items():
        if field.get(k) != v:
            problems.append(f'field endpoint {k} != {v!r}')
    if cp in CHECKPOINTS[3:] and field.get('manager_60') == field.get('resource_48'):
        problems.append('atlas/environment owners must be distinct')
    if cp in CHECKPOINTS[5:] and not field.get('ruw_slot0_live'):
        problems.append('RUW slot0 endpoint absent')
    return problems

def object_json(path):
    v = parse_json(read_text(path), str(path))
    if not isinstance(v, dict):
        raise Malformed(f'{path}: expected an object')
    return v

def load_field(path, native=False):
    v = object_json(path)
    missing = set(FIELD_KEYS) - set(v)
    if set(v) - set(FIELD_KEYS) or (missing and not (native and missing == {'reserve_class_3abb14'})):
        raise Malformed(f'{path}: field keys missing {sorted(missing)}, unknown {sorted(set(v) - set(FIELD_KEYS))}')
    for k in FIELD_BOOL:
        if type(v[k]) is not bool:
            raise Malformed(f'{path}: {k} must be boolean')
    for k in FIELD_INT:
        if k in v and not is_int(v[k]):
            raise Malformed(f'{path}: {k} must be integer')
    if not _typed(v['tex0_332430'], 'hex64') or not _typed(v['region_332450'], 'int4'):
        raise Malformed(f'{path}: TEX0/region format')
    if v['ruw_sha256'] is not None and not hex_string(v['ruw_sha256'], 64):
        raise Malformed(f'{path}: RUW SHA256')
    return v, sorted(missing)

def required_original_files():
    return ['reference.json'] + [f'{cp}/{name}' for cp in CHECKPOINTS for name in ORIGINAL_FILES]

def verify_inventory(original, path):
    m = object_json(path)
    if set(m) != {'schema', 'elf_sha256', 'checkpoints', 'files'} or m['schema'] != 'okage.field-prefix-pins.v1':
        raise Malformed('inventory schema/keys')
    elf = object_json(Path(__file__).resolve().parents[1] / 'docs/disc-report.json')['elf']['sha256']
    if m['elf_sha256'] != elf:
        raise Malformed('inventory ELF identity differs from docs/disc-report.json')
    if m['checkpoints'] != list(CHECKPOINTS):
        raise Malformed('inventory checkpoint coverage/order')
    if not isinstance(m['files'], dict) or set(m['files']) != set(required_original_files()):
        raise Malformed('inventory must pin reference.json and every required checkpoint file exactly')
    for rel in required_original_files():
        pin = m['files'][rel]
        if not isinstance(pin, dict) or set(pin) != {'sha256', 'bytes'} or not hex_string(pin['sha256'], 64) \
                or not is_int(pin['bytes']) or pin['bytes'] < 0:
            raise Malformed(f'inventory malformed pin: {rel}')
        try:
            data = (original / rel).read_bytes()
        except OSError as exc:
            raise Malformed(str(exc)) from exc
        if len(data) != pin['bytes'] or hashlib.sha256(data).hexdigest() != pin['sha256']:
            raise Malformed(f'{rel}: does not match pinned original SHA256/size')
    return m

def refuse_self_compare(original, native):
    o, n = original.resolve(), native.resolve()
    if o == n or o in n.parents or n in o.parents:
        raise Malformed('refusing same or nested original/native directories')
    originals = {(p.stat().st_dev, p.stat().st_ino) for p in (o / rel for rel in required_original_files())}
    for cp in CHECKPOINTS:
        for name in NATIVE_FILES:
            p = n / cp / name
            if p.exists() and (p.stat().st_dev, p.stat().st_ino) in originals:
                raise Malformed(f'refusing self-comparison through shared file: {cp}/{name}')

def load_reference(path):
    r = object_json(path)
    if r.get('schema') != 'okage.field-prefix-original-reference.v1' or r.get('route') != 'newgame-field-prefix':
        raise Malformed('reference schema/route')
    if not isinstance(r.get('checkpoints'), dict) or list(r['checkpoints']) != list(CHECKPOINTS):
        raise Malformed('reference checkpoint coverage/order')
    if not isinstance(r.get('problems'), list) or any(not isinstance(x, str) for x in r['problems']):
        raise Malformed('reference problems missing/malformed')
    for key in ('determinism', 'newgame_neutrality'):
        if not isinstance(r.get(key), dict) or not r[key]:
            raise Malformed(f'reference missing {key} evidence')
    def has_false(v):
        return v is False or (isinstance(v, dict) and any(has_false(x) for x in v.values()))
    if has_false(r['determinism']) or has_false(r['newgame_neutrality']):
        raise Malformed('reference determinism/Newgame neutrality failure')
    if not isinstance(r.get('control_minimal'), dict) or not isinstance(r['control_minimal'].get('failures'), list) \
            or any(not isinstance(x, str) for x in r['control_minimal']['failures']):
        raise Malformed('reference minimal control missing/malformed')
    neutral_files = {'events.jsonl', 'state.json', 'rng.jsonl', 'session.json', 'effects.json'}
    if set(r['newgame_neutrality']) != set(h.CHECKPOINTS) or any(not isinstance(v, dict) or set(v) != neutral_files
            or any(x is not True for x in v.values()) for v in r['newgame_neutrality'].values()):
        raise Malformed('reference inherited Newgame neutrality missing or unsuccessful')
    for cp in CHECKPOINTS:
        source_cp = 'ui-publish-pre' if cp == 'ui-prefix-published' else cp
        for suffix in ('state (fill fields removed)', 'vram-last-writer.bin', 'vram-upload-ever.bin', 'vram.bin within fill'):
            d = r['determinism'].get(f'checkpoint {source_cp}: {suffix}')
            if not isinstance(d, dict) or d.get('identical') is not True:
                raise Malformed(f'reference determinism missing for {source_cp}: {suffix}')
    rng = r.get('rng', {})
    if not str(rng.get('provenance', '')).startswith('every draw is a hooked') or rng.get('total') != RNG_COUNT or rng.get('window_draws') != 0:
        raise Malformed('reference every-draw RNG provenance/count missing')
    control = r['control_minimal'].get('checkpoints_compared')
    expected_controls = {'field-constructor-return', 'worker-entry', 'ui-publish-pre', 'ui-publish-post-block'}
    if not isinstance(control, dict) or set(control) != expected_controls:
        raise Malformed('reference minimal control checkpoint evidence missing')
    for cp, evidence in control.items():
        required = {'resource_state_equal', 'session_state_equal', 'field_equal_except_counter', 'vram_fill00_equal'}
        if not isinstance(evidence, dict) or not required <= set(evidence):
            raise Malformed(f'reference minimal control evidence missing for {cp}')
        if any(not isinstance(evidence[k], dict) or not evidence[k] for k in ('resource_state_equal', 'field_equal_except_counter')):
            raise Malformed(f'reference minimal control state coverage missing for {cp}')
        if cp.startswith('ui-publish') and type(evidence.get('ee_ram_equal')) is not bool:
            raise Malformed(f'reference minimal control RAM evidence missing for {cp}')
        for key in required | ({'ee_ram_equal'} if cp.startswith('ui-publish') else set()):
            value = evidence[key]
            failed = value is False or (isinstance(value, dict) and any(x is not True for x in value.values()))
            if failed != (f'{cp}:{key}' in r['control_minimal']['failures']):
                raise Malformed(f'reference minimal control failure witness inconsistent for {cp}:{key}')
    if r['problems'] != ['control:' + x for x in r['control_minimal']['failures']]:
        raise Malformed('reference problems do not correspond to minimal-control failures')
    if r['control_minimal'].get('missing') != []:
        raise Malformed('reference minimal control has missing endpoints')
    p = r.get('publish_proof')
    if not isinstance(p, dict) or p.get('changed_words_outside_block_count') != 0 \
            or p.get('resource_state_equal') is not True or p.get('vram_equal') is not True:
        raise Malformed('reference bracket publication proof missing/failing')
    if not isinstance(p.get('stores'), dict) or not p['stores'] or any(
            not isinstance(x, dict) or x.get('ok') is not True for x in p['stores'].values()):
        raise Malformed('reference bracket stores missing/failing')
    if p['stores'].get('0x34') != {'before': 0, 'after': 1, 'expected': 1, 'ok': True}:
        raise Malformed('reference publication store proof missing/corrupted')
    s = r.get('scheduling')
    if not isinstance(s, dict) or not isinstance(s.get('field_constructor_entry'), dict) \
            or not isinstance(s.get('checkpoints'), dict) or set(s['checkpoints']) != set(CHECKPOINTS):
        raise Malformed('reference scheduling checkpoint coverage')
    rows = [s['field_constructor_entry']] + [s['checkpoints'][cp] for cp in CHECKPOINTS]
    for k in ('vb', 'traversal'):
        if any(not isinstance(row, dict) or not is_int(row.get(k)) or row[k] < 0 for row in rows):
            raise Malformed(f'reference scheduling {k} missing/malformed')
        if any(a[k] > b[k] for a, b in zip(rows, rows[1:])):
            raise Malformed(f'reference scheduling {k} decreases')
    for cp in CHECKPOINTS:
        c = r['checkpoints'][cp]
        if not isinstance(c, dict) or any(c.get(k) != s['checkpoints'][cp][k] for k in ('vb', 'traversal')):
            raise Malformed(f'reference {cp}: scheduling inconsistent')
        if not is_int(c.get('resource_events')) or not is_int(c.get('rng_draws_before')):
            raise Malformed(f'reference {cp}: missing event/RNG counts')
    return r

def load_route(path, elf):
    r = object_json(path)
    if set(r) != {'route', 'elf_sha256', 'checkpoints'} or r['route'] != 'newgame-field-prefix':
        raise Malformed('field-route.json schema/route')
    if r['elf_sha256'] != elf:
        raise Malformed('native ELF identity differs')
    c = r['checkpoints']
    expected = h.CHECKPOINTS + CHECKPOINTS
    if not isinstance(c, dict) or set(c) != set(expected) or any(not is_int(x) or x < 0 for x in c.values()):
        raise Malformed('field-route.json checkpoint coverage/frames')
    if any(c[a] > c[b] for a, b in zip(expected, expected[1:])):
        raise Malformed('field-route.json checkpoint frames decrease')
    return r

def first_difference(a, b):
    if isinstance(a, list) and isinstance(b, list):
        for i in range(max(len(a), len(b))):
            if i >= len(a) or i >= len(b) or canon(a[i]) != canon(b[i]):
                return {'index': i, 'original': a[i] if i < len(a) else None,
                        'native': b[i] if i < len(b) else None, 'lengths': [len(a), len(b)]}
    elif canon(a) != canon(b):
        return {'original': a, 'native': b}
    return None

def compare_vram(original, native, size=VRAM):
    om = base.read_exact(original / 'vram-written-mask.bin', size)
    ov = base.read_exact(original / 'vram-written-values.bin', size)
    oi = base.read_exact(original / 'vram-invalidated-mask.bin', size)
    nm = base.read_exact(native / 'vram.known', size)
    nv = base.read_exact(native / 'vram.bytes', size)
    ni = base.read_exact(native / 'vram.invalidated', size)
    for name, data in [('original known', om), ('original invalidated', oi), ('native known', nm), ('native invalidated', ni)]:
        base._two_bit(data, name)
    as_int = lambda data: int.from_bytes(data, 'little')
    omi, nmi, oii, nii = map(as_int, (om, nm, oi, ni))
    if omi & oii or nmi & nii:
        raise Malformed('VRAM known and invalidated nibble masks overlap')
    xor_values = (as_int(ov) ^ as_int(nv)).to_bytes(size, 'little')
    nonzero_nibbles = bytes(bool(x & 15) | (bool(x & 240) << 1) for x in range(256))
    differences = {'known_mask_mismatch': omi ^ nmi,
                   'invalidated_mask_mismatch': oii ^ nii,
                   'value_mismatch': as_int(xor_values.translate(nonzero_nibbles)) & omi & nmi}
    counts, first = {}, {}
    for key, mask in differences.items():
        counts[key] = sum(mask.to_bytes(size, 'little').translate(bytes(bin(x).count('1') for x in range(256))))
        if mask:
            bit = (mask & -mask).bit_length() - 1
            first[key] = {'byte': bit // 8, 'nibble': 'low' if bit % 8 == 0 else 'high'}
    return {**counts, 'first': first}

def compare(original, native, inventory, vram_size=VRAM):
    original, native = Path(original), Path(native)
    pins = verify_inventory(original, inventory)
    refuse_self_compare(original, native)
    ref = load_reference(original / 'reference.json')
    missing = [f'{cp}/{name}' for cp in CHECKPOINTS for name in NATIVE_FILES if not (native / cp / name).is_file()]
    if not (native / 'field-route.json').is_file():
        missing.append('field-route.json')
    result = {'schema': 'okage.field-prefix-comparison.v1', 'checkpoints': {}, 'incomplete': missing,
              'payload': 'UNVERIFIED', 'timing': {'status': 'UNVERIFIED'},
              'reference_unmodeled_source_effects': ref.get('unmodeled_source_effects', []),
              'reference_gate': {'status': 'UNPROVEN' if ref['problems'] else 'VERIFIED', 'problems': ref['problems']},
              'limitations': ['Only the seven field-prefix checkpoints are covered; later field, battle and Save/Restore are not proved.',
                              'Timing is a separate gate; differences never normalize payload.',
                              'Phase markers and documented diagnostic-only state/effect fields are excluded.',
                              'Undefined VRAM nibble values are not compared; both masks must still match exactly.']}
    if missing:
        return 5, dict(result, status='INCOMPLETE')
    route = load_route(native / 'field-route.json', pins['elf_sha256'])
    anchor = route['checkpoints']['field-constructor-entry']
    source_anchor = ref['scheduling']['field_constructor_entry']
    timing = {}
    for cp in CHECKPOINTS:
        source = ref['scheduling']['checkpoints'][cp]
        timing[cp] = {'original_vblank_delta': source['vb'] - source_anchor['vb'],
                      'original_traversal_delta': source['traversal'] - source_anchor['traversal'],
                      'native_frame_delta': route['checkpoints'][cp] - anchor}
    # Native Session frame means one scheduler traversal; vblank is retained, not silently equated to it.
    timing_equal = all(v['original_traversal_delta'] == v['native_frame_delta'] for v in timing.values())
    result['timing'] = {'status': 'IDENTICAL' if timing_equal else 'DIFFERENT', 'checkpoints': timing,
                        'rule': 'Compare scheduler traversal/native frame deltas from field-constructor-entry; retain vblank deltas separately.'}
    prior = {}
    different, asserted = False, False
    for cp in CHECKPOINTS:
        o, n = original / cp, native / cp
        oe, _ = load_lifecycle(o / 'events.jsonl')
        ne, na = load_lifecycle(n / 'lifecycle.jsonl', native=True)
        oe, ne = [x for x in oe if x['kind'] != 'phase'], [x for x in ne if x['kind'] != 'phase']
        org, _ = h.load_rng(o / 'rng.jsonl')
        nrg, nra = h.load_rng(n / 'rng.jsonl', native=True)
        os_, ns = load_state(o / 'state.json', cp), load_state(n / 'state.json', cp)
        of, _ = load_field(o / 'field.json')
        nf, miss = load_field(n / 'field.json', native=True)
        result['incomplete'].extend(f'{cp}/field.json:{k}' for k in miss)
        oc, nc = coverage(cp, oe, org, of), coverage(cp, ne, nrg, nf)
        if oc:
            raise Malformed(f'original {cp} incomplete source coverage: {oc}')
        result['incomplete'].extend(f'{cp}:{problem}' for problem in nc)
        for k, rows in [('original_events', oe), ('native_events', ne), ('original_rng', org), ('native_rng', nrg)]:
            if k in prior and canon(rows[:len(prior[k])]) != canon(prior[k]):
                if k.startswith('original'):
                    raise Malformed(f'{cp}: pinned original {k} stream does not extend preceding checkpoint')
                result['checkpoints'].setdefault(cp, {})[k + '_stream_inconsistent'] = True
                different = True
            prior[k] = rows
        if ref['checkpoints'][cp]['resource_events'] != len(oe) or ref['checkpoints'][cp]['rng_draws_before'] != len(org) \
                or canon(ref['checkpoints'][cp].get('field')) != canon(of):
            raise Malformed(f'{cp}: reference event/RNG/field digest does not describe pinned payload')
        vd = ref['checkpoints'][cp].get('vram', {})
        for name, key in (('vram-written-mask.bin', 'mask_sha256'), ('vram-written-values.bin', 'values_sha256'),
                          ('vram-invalidated-mask.bin', 'invalidated_sha256')):
            if hashlib.sha256((o / name).read_bytes()).hexdigest() != vd.get(key):
                raise Malformed(f'original {cp} VRAM {key} digest mismatch')
        for name, key in (('vram-written-mask.bin', 'known_nibbles_required'), ('vram-invalidated-mask.bin', 'unknown_nibbles_required')):
            if sum((o / name).read_bytes().translate(base._POP)) != vd.get(key):
                raise Malformed(f'original {cp} VRAM coverage digest mismatch')
        payload = {'coverage_problems': nc, 'missing_field_keys': miss, 'lifecycle': first_difference(oe, ne), 'rng': first_difference(org, nrg),
                   'state': {k: first_difference(os_[k], ns[k]) for k in STATE_KEYS if canon(os_[k]) != canon(ns[k])},
                   'effects': first_difference(base.load_effects(o / 'effects.json'), base.load_effects(n / 'effects.json')),
                   'session': first_difference(h.load_session(o / 'session.json'), h.load_session(n / 'session.json')),
                   'field': {k: first_difference(of[k], nf[k]) for k in FIELD_KEYS if k in nf and canon(of[k]) != canon(nf[k])},
                   'vram': compare_vram(o, n, vram_size), 'asserted_rows': sum(na) + sum(nra)}
        if any(payload[k] for k in ('lifecycle', 'rng', 'state', 'effects', 'session', 'field')) \
                or any(payload['vram'][k] for k in ('known_mask_mismatch', 'invalidated_mask_mismatch', 'value_mismatch')):
            different = True
        asserted = asserted or payload['asserted_rows'] > 0
        result['checkpoints'].setdefault(cp, {}).update(payload)
    result['payload'] = 'DIFFERENT' if different else ('INCOMPLETE' if result['incomplete'] else ('UNPROVEN' if asserted else 'IDENTICAL'))
    code, status = ((5, 'INCOMPLETE') if result['incomplete'] else (1, 'DIFFERENT') if different else
                    (4, 'UNPROVEN') if asserted or ref['problems'] else (0, 'COMPONENT-IDENTICAL'))
    result['payload_identical'] = not different and not result['incomplete']
    result['whole_timeline_parity'] = False
    return code, dict(result, status=status)

def run(original, native, inventory, vram_size=VRAM):
    try:
        return compare(original, native, inventory, vram_size)
    except (Malformed, OSError, UnicodeError) as exc:
        return 2, {'schema': 'okage.field-prefix-comparison.v1', 'status': 'MALFORMED', 'error': str(exc)}

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('original')
    p.add_argument('native')
    p.add_argument('--inventory', required=True)
    p.add_argument('--json')
    args = p.parse_args(argv)
    code, result = run(args.original, args.native, args.inventory)
    if args.json:
        with open(args.json, 'x') as out:
            out.write(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return code

if __name__ == '__main__':
    sys.exit(main())
