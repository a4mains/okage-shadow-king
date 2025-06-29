#!/usr/bin/env python3
"""Extract the Retire title-retirement original reference from `okage-startup-oracle --title-retire ROUTE` captures.

usage: retire_title_original_extract.py EVIDENCE_DIR --out ANALYSIS_DIR --rng RNG_JSONL
           [--title-runs TITLE_RUNS_DIR --title-analysis TITLE_ANALYSIS_DIR] [--startup-run STARTUP_FILL00_A]
           [--regress-title RUN_DIR] [--regress-default RUN_DIR]

EVIDENCE_DIR/runs/<route>/{fill00-a,fill00-b,fillff-a,fillff-b,control-minimal-fill00} for route in restore, newgame
(tools/retire_title_capture.sh). --rng is the RNG original rng.jsonl (its phase labels are reused for the 9,504
startup draws after verifying state/value equality; later draws are labelled "session").
--title-runs/--title-analysis: the pinned Title evidence, for the hook-neutrality check at the Title stop.
--regress-title/--regress-default: runs of the SAME Retire oracle binary in `--title` and default mode, checked
byte-identical (modulo documented host-timing fields) to the Title fill00-a and the Startup fill00-a captures.

Writes ANALYSIS_DIR (must not exist):
  <route>/reference.json            checkpoints, pad script/consumption, title phases, effect106 history, mode and
                                    retirement sequences, RNG, survivors and pending tails, determinism, controls
  <route>/events-detail.jsonl       the whole ordered timeline with source attribution
  <route>/pad-consumption.jsonl     every mode-8 pad read (0x12bfc8): vblank, repeat/pressed words, analog bytes
  <route>/title-frames.jsonl        every 0x1296f8 entry: title_n, vblank, submode, counter, selection, words, water
  <route>/<checkpoint>/events.jsonl  ordered timeline prefix up to the checkpoint (native lifecycle schema)
  <route>/<checkpoint>/state.json    allocator/handle/record/list state (native state.json schema, Retire coverage)
  <route>/<checkpoint>/effects.json  {"effect106": ...}
  <route>/<checkpoint>/rng.jsonl     every rand() draw before the checkpoint (RNG native schema)
  <route>/<checkpoint>/vram-written-mask.bin, vram-written-values.bin, vram-invalidated-mask.bin   (as Title)
  regression.json                   (when --regress-* given)
Reads only; exits 1 if the runs disagree or a cross-check fails. Research evidence only; never product input.
"""
import argparse
import collections
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import title_original_extract as title  # noqa: E402

VRAM = 1 << 22
ROUTES = ('restore', 'newgame')
RUNS = ('fill00-a', 'fill00-b', 'fillff-a', 'fillff-b')
CONTROL = 'control-minimal-fill00'
CHECKPOINTS = ('first-update', 'stable-menu', 'removal-request', 'resource-retirement', 'successor-entry')
TRIGGER = {'first-update': 'title_update', 'stable-menu': 'title_update', 'removal-request': 'effect_remove_request',
           'resource-retirement': 'mode8_destroy', 'successor-entry': 'successor_construct'}
ROUTE_MODE = {'restore': 10, 'newgame': 9}
MEMORY = 'host-uploads-known-draw-writes-unknown'
PAD_SITES = {'0x12c014': 'repeat', '0x12c044': 'analog', '0x12c1d8': 'pressed'}
NIGHT_V_PTR_FIELD = 'NIGHT_V material table pointer (0x182e28 inside 0x12c350 selector 1)'

Fail = title.Fail
need = title.need
sha = title.sha
fsha = title.fsha
read_jsonl = title.read_jsonl
jl = title.jl

def write_x(path, text):
    with open(path, 'x') as f:
        f.write(text)

def effect_state(fx):
    """effect106 summary from the observer's effectJson (None = not created / slot unknown)."""
    if fx is None:
        return None
    job = fx.get('job')
    if job == -1:
        return {'slot': fx['slot'], 'shape': None, 'state': 'reclaimed', 'element_handles': []}
    flags = int(fx['flags'], 16)
    st = 'reclaim-pending' if flags & 0x200 else 'removal-requested' if flags & 0x100 else 'active'
    return {'slot': fx['slot'], 'shape': fx['type'], 'state': st, 'element_handles': fx['element_handles']}

def timeline(events):
    enters = {}
    out, detail = [], []
    live = set()
    archive = xmd_path = None
    counts = collections.Counter()
    title_n = None
    last_sub = None

    def add(row, e, ent=None):
        out.append(row)
        d = dict(row)
        src = ent or e
        d.update({'oracle_i': e['i'], 'vb': e.get('vb'), 'top': e.get('top'), 'fn': src.get('fn'),
                  'caller_ra': src.get('ra'), 'parent': src.get('parent'), 'title_n': title_n})
        detail.append(d)
        counts[row['kind'] if row['kind'] not in ('effect', 'mode') else row['kind'] + ':' + row['op']] += 1

    for e in events:
        ev = e['ev']
        if ev == 'enter':
            enters[e['i']] = e
            n = e['name']
            if n == 'archive_mount':
                archive = e.get('str')
            elif n == 'xmd_load_path':
                xmd_path = e.get('str')
            elif n == 'upload':
                add({'kind': 'upload', 'alloc': e['desc']['id']}, e)
            elif n == 'handle_retire':
                if e.get('record', -1) >= 0:
                    add({'kind': 'retire', 'handle': int(e['handle_or_record'])}, e)
                else:
                    counts['retire_noop'] += 1
            elif n == 'palette_bind':
                if e.get('record', -1) >= 0:
                    raw = int(e['desc']['raw'], 16)
                    add({'kind': 'bind', 'handle': e['handle'], 'level': e['level'],
                         'alloc': e['desc']['id'] if raw else -1}, e)
                else:
                    counts['bind_noop'] += 1
            elif n == 'handle_name':
                if e['handle'] in live:
                    add({'kind': 'name', 'handle': e['handle'], 'name': e['name_str']}, e)
                else:
                    counts['name_noop'] += 1
            elif n == 'mode8_construct':
                add({'kind': 'phase', 'name': 'mode8-selector%d-construct' % e['selector']}, e)
            elif n == 'logo_tick' and e['tick'] in (1, 180, 270, 480, 600):
                add({'kind': 'phase', 'name': 'logo-tick-%d' % e['tick']}, e)
            elif n == 'resource_retire':
                need(e.get('mode8_selector', -1) >= 0, 'resource child retired without a mode-8 parent')
                add({'kind': 'mode', 'op': 'resource-retire', 'mode': 8, 'selector': e['mode8_selector']}, e)
            elif n == 'mode8_destroy':
                add({'kind': 'mode', 'op': 'destroy', 'mode': 8, 'selector': e['selector']}, e)
            elif n == 'mode_request':
                add({'kind': 'mode', 'op': 'request', 'mode': e['mode'], 'selector': e['selector'], 'arg': e['arg3']}, e)
            elif n == 'host_free' and 'scheduler_node' in e:
                node = e['scheduler_node']
                need(node['kind'] in ('mode8', 'resource') and node['mode8_selector'] >= 0,
                     f'scheduler freed an unmodelled node {node}')
                add({'kind': 'mode', 'op': 'wrapper-free', 'node': node['kind'], 'mode': 8,
                     'selector': node['mode8_selector']}, e)
            elif n == 'effect_remove_request':
                add({'kind': 'effect', 'op': 'remove-request', 'slot': e['slot']}, e)
            elif n == 'effect_free' and e.get('job_slot', -1) >= 0:
                add({'kind': 'effect', 'op': 'reclaim', 'slot': e['job_slot']}, e)
            elif n == 'title_update':
                title_n = e['title_n']
                if e['submode'] != last_sub:
                    add({'kind': 'phase', 'name': 'title-submode-%d' % e['submode']}, e)
                    last_sub = e['submode']
            elif n == 'successor_construct':
                add({'kind': 'phase', 'name': 'successor-0x124548-entry-mode%d' % e['parent_4c']}, e)
        elif ev == 'exit':
            ent = enters[e['enter']]
            n = e['name']
            if n == 'alloc':
                r = e['result']
                add({'kind': 'alloc', 'alloc': r['id'] if r else -1, 'class': ent['class'], 'psm': ent['psm'],
                     'w': ent['w'], 'h': ent['h'], 'tbp': r['tbp'] if r else -1}, e, ent)
            elif n == 'retain':
                if int(ent['desc']['raw'], 16):
                    add({'kind': 'ref', 'alloc': ent['desc']['id'], 'delta': ent['delta'], 'refs': e['refs_after']}, e, ent)
                else:
                    counts['ref_null'] += 1
            elif n == 'free':
                if e['freed']:
                    add({'kind': 'free', 'alloc': ent['desc']['id']}, e, ent)
                else:
                    counts['free_noop'] += 1
            elif n in ('handle_create', 'handle_alias'):
                h = int(e['handle'], 0) if isinstance(e['handle'], str) else e['handle']
                add({'kind': 'handle', 'handle': h, 'record': e['record'] if h >= 0 else -1,
                     'alias': n == 'handle_alias'}, e, ent)
                if h >= 0:
                    live.add(h)
            elif n == 'handle_retire':
                if ent.get('record', -1) >= 0:
                    live.discard(int(ent['handle_or_record']))
            elif n == 'handle_reserve':
                add({'kind': 'reserve', 'count': ent['count'], 'first': e['first'], 'class': ent['reserve_class']}, e, ent)
            elif n == 'material_table':
                t = e['table']
                need(t is not None, 'material table pointer not readable')
                if t['handles']:
                    need(archive and xmd_path, 'material table without an archive/xmd path context')
                    stem = os.path.splitext(os.path.basename(archive))[0].upper()
                for i, h in enumerate(t['handles']):
                    add({'kind': 'material', 'model': stem + '/' + xmd_path, 'slot': i, 'handle': h}, e, ent)
            elif n == 'mode8_construct':
                add({'kind': 'phase', 'name': 'mode8-selector%d-constructed' % e['selector']}, e, ent)
            elif n == 'effect_alloc':
                need(int(e['v0'], 16) == e['slot'], 'effect slot return mismatch')
                add({'kind': 'effect', 'op': 'create', 'shape': ent['shape'], 'slot': e['slot']}, e, ent)
            elif n == 'effect_bind':
                fx = e['effect']
                need(fx is not None and fx['element_handles'][ent['element']] == ent['handle'],
                     '0x10c188 did not bind the requested handle (job type check failed?)')
                add({'kind': 'effect', 'op': 'bind', 'slot': ent['slot'], 'element': ent['element'],
                     'handle': ent['handle']}, e, ent)
        elif ev == 'prefix_end':
            add({'kind': 'phase', 'name': 'prefix-end-0x100618'}, e)
        elif ev == 'checkpoint':
            add({'kind': 'phase', 'name': 'checkpoint-' + e['name']}, e)
        elif ev == 'stop':
            add({'kind': 'phase', 'name': 'stop-' + e['kind']}, e)
    need(all(a['oracle_i'] <= b['oracle_i'] for a, b in zip(detail, detail[1:])), 'timeline not in source order')
    return out, detail, counts

def checkpoint_boundaries(ev):
    """{name: (checkpoint event, boundary oracle index)}. A checkpoint taken at a function entry precedes every
    event of that call: the boundary is the triggering entry's index (exclusive)."""
    out = {}
    by_i = {e['i']: e for e in ev}
    for e in ev:
        if e['ev'] != 'checkpoint':
            continue
        need(e['name'] not in out, 'checkpoint repeated: ' + e['name'])
        if e['name'] == 'title-constructed':
            out[e['name']] = (e, e['i'])
            continue
        prev = by_i[e['i'] - 1]
        need(prev['ev'] == 'enter' and prev['name'] == TRIGGER[e['name']] and prev['fn'] == e['pc'],
             f'checkpoint {e["name"]} is not directly after its {TRIGGER[e["name"]]} entry')
        out[e['name']] = (e, prev['i'])
    return out

def prefix(rows_detail, rows, boundary):
    k = 0
    while k < len(rows_detail) and rows_detail[k]['oracle_i'] < boundary:
        k += 1
    return rows[:k]

def rng_draws(ev, rng):
    draws, detail = [], []
    pending = None
    prefix_seen = False
    for e in ev:
        if e['ev'] == 'prefix_end':
            prefix_seen = True
        elif e['ev'] == 'rand':
            need(pending is None, 'nested rand')
            pending = e
        elif e['ev'] == 'rand_exit':
            need(pending is not None and e['n'] == pending['n'], 'rand_exit without rand')
            i = len(draws)
            if i < len(rng):
                need(rng[i]['state'] == e['state'] and rng[i]['value'] == int(e['v0']) & 0xffffffff,
                     f'draw {i} differs from the RNG original')
                phase = rng[i]['phase']
            else:
                need(prefix_seen, 'more startup draws than RNG recorded')
                phase = 'session'
            draws.append({'i': i, 'phase': phase, 'state': e['state'], 'value': int(e['v0']) & 0xffffffff})
            detail.append({'i': i, 'oracle_i': pending['i'], 'vb': pending.get('vb'), 'ra': pending['ra'],
                           'frames': pending.get('frames'), 'state_before': pending['state'], 'state_after': e['state'],
                           'value': int(e['v0']) & 0xffffffff, 'after_prefix': prefix_seen})
            pending = None
    need(pending is None, 'rand pending at the stop')
    return draws, detail

def native_state(fs, cp, table_ptr):
    """State at a checkpoint in the native state.json schema; materials = the NIGHT_V table while it is listed
    (0x330144 list), [] once 0x179650 has unlinked and freed it."""
    serial = {i: x for i, x in fs['allocation_serials']}
    live_allocs = []
    for a in fs['allocations']:
        if not a['live']:
            continue
        d = a['desc']
        live_allocs.append({'id': a['id'], 'serial': serial[a['id']], 'class': a['class'], 'psm': d['psm'],
                            'width': d['w'], 'height': d['h'], 'tbp': d['tbp'], 'tbw': d['tbw'],
                            'references': a['refs'], 'descriptor': int(d['raw'], 16)})
    live_allocs.sort(key=lambda a: a['id'])
    live_ids = {a['id'] for a in live_allocs}

    def ref(i):
        if i < 0:
            return None
        need(i in live_ids, f'record refers to allocation {i} that is not live')
        return [i, serial[i]]
    recs = {r['r']: r for r in fs['records']}
    handles = []
    for h, r in fs['handles']:
        rec = recs[r]
        handles.append({'handle': h, 'record': r, 'image': ref(rec['image']),
                        'palettes': [ref(rec['pal0'])] + [ref(x) for x in rec['extra']]})
    records = [{'record': r['r'], 'name': r['name'], 'hash_slot': r['hash_slot'], 'handles': r['handles']}
               for r in sorted(fs['records'], key=lambda r: r['r'])]
    tables = [t for t in fs['material_tables'] if t and int(t['ptr'], 16) == table_ptr]
    need(len(tables) <= 1, 'NIGHT_V material table listed twice')
    need(fs['reserve_class'] == 0 and fs['reserved1'] == [],
         'class-1 reserved queue in use at a checkpoint: native schema carries only the class-0 queue')
    return {
        'coverage': {'lifecycle': 'retire-title', 'checkpoint': cp, 'memory': MEMORY},
        'allocations': live_allocs, 'handles': handles, 'records': records,
        'hash': fs['hash'], 'reserved': fs['reserved'], 'free_records': fs['free_records'],
        'free_extras': fs['free_extras'], 'available': fs['vram']['available'], 'cursors': fs['vram']['cursors'],
        'free_queue_ids': fs['free_queue_ids'], 'occupancy_hex': fs['occupancy_hex'],
        'image_list': fs['image_list'], 'materials': [[i, h] for i, h in enumerate(tables[0]['handles'])] if tables else [],
    }

# ----------------------------------------------------------------------------------------------
def vram_analysis(f00, fff, writer_path, ever_path, out):
    """Title paired-fill rule at one checkpoint (files of fill00-a/fillff-a)."""
    v00 = open(f00, 'rb').read()
    vff = open(fff, 'rb').read()
    writer = open(writer_path, 'rb').read()
    ever = open(ever_path, 'rb').read()
    need(len(v00) == len(vff) == len(writer) == len(ever) == VRAM, 'VRAM images must be 4 MiB')
    U, T, vals = bytearray(VRAM), bytearray(VRAM), bytearray(VRAM)
    st = collections.Counter()
    first = None
    for i in range(VRAM):
        a, b, w, e = v00[i], vff[i], writer[i], ever[i]
        if not (a | w | e) and b == 0xff:
            st['never'] += 2
            continue
        u = t = v = 0
        for shift, bit in ((0, 1), (4, 2)):
            na, nb = (a >> shift) & 0xf, (b >> shift) & 0xf
            code = (w >> shift) & 0xf
            paired = na == nb
            if not paired and not (na == 0 and nb == 0xf):
                st['sub_nibble_anomalies'] += 1
            if paired != bool(e & bit):
                st['paired_vs_ledger_upload_mismatch'] += 1
                if first is None:
                    first = (i, bit, a, b, w, e)
            if code == 1:
                st['last_upload'] += 1
                if paired:
                    u |= bit
                    v |= na << shift
            elif code in (2, 3):
                t |= bit
                st['last_draw' if code == 2 else 'last_local_to_local'] += 1
                if paired:
                    st['uploaded_then_invalidated'] += 1
            elif code == 0:
                st['never'] += 1
                if paired:
                    st['paired_written_without_ledger_writer'] += 1
            else:
                st['unknown_writer_code'] += 1
        U[i], T[i], vals[i] = u, t, v
    need(st['paired_vs_ledger_upload_mismatch'] == 0,
         f'paired-fill written mask differs from the ledger upload footprint ({st["paired_vs_ledger_upload_mismatch"]}, first {first})')
    need(st['unknown_writer_code'] == 0 and st['sub_nibble_anomalies'] == 0, f'VRAM anomalies: {dict(st)}')
    for name, data in (('vram-written-mask.bin', U), ('vram-written-values.bin', vals), ('vram-invalidated-mask.bin', T)):
        with open(os.path.join(out, name), 'xb') as f:
            f.write(bytes(data))
    return {'known_nibbles_required': title.nibbles(U), 'unknown_nibbles_required': title.nibbles(T),
            'ever_uploaded_nibbles': title.nibbles(ever), 'stats': dict(st),
            'mask_sha256': sha(bytes(U)), 'values_sha256': sha(bytes(vals)), 'invalidated_sha256': sha(bytes(T))}

def strip_fill(s):
    d = json.loads(json.dumps(s))
    d['vram'].pop('run_fill', None)
    d['vram'].pop('run_vram_sha256', None)
    return d

def determinism(runs, cps):
    res = {}
    for name in ('events.jsonl', 'disc-opens.jsonl', 'pad-host.jsonl', 'vram-last-writer.bin', 'vram-upload-ever.bin',
                 'ee-ram-32mb.bin', 'ee-ram-after-10bc78.bin'):
        hs = {r: fsha(os.path.join(p, name)) for r, p in runs.items()}
        res[name] = {'identical': len(set(hs.values())) == 1, 'sha256': hs}
    led = {r: title.ledger_without_ee_cross_reference(os.path.join(p, 'gs-ledger.jsonl')) for r, p in runs.items()}
    res['gs-ledger.jsonl (ee_event cross-reference removed)'] = {'identical': all(led[r] == led['fill00-a'] for r in led)}
    for f in ('final-state.jsonl', 'state-at-100618.jsonl'):
        ss = {r: strip_fill(read_jsonl(os.path.join(p, f))[0]) for r, p in runs.items()}
        res[f + ' (fill fields removed)'] = {'identical': all(ss[r] == ss['fill00-a'] for r in ss)}
    vr = {r: fsha(os.path.join(p, 'vram.bin')) for r, p in runs.items()}
    res['vram.bin within fill'] = {'identical': vr['fill00-a'] == vr['fill00-b'] and vr['fillff-a'] == vr['fillff-b']}
    for cp in cps:
        d = {r: os.path.join(p, 'checkpoints', cp) for r, p in runs.items()}
        ss = {r: strip_fill(read_jsonl(os.path.join(x, 'state.jsonl'))[0]) for r, x in d.items()}
        res[f'checkpoint {cp}: state (fill fields removed)'] = {'identical': all(ss[r] == ss['fill00-a'] for r in ss)}
        for name in ('vram-last-writer.bin', 'vram-upload-ever.bin'):
            hs = {r: fsha(os.path.join(x, name)) for r, x in d.items()}
            res[f'checkpoint {cp}: {name}'] = {'identical': len(set(hs.values())) == 1}
        vr = {r: fsha(os.path.join(x, 'vram.bin')) for r, x in d.items()}
        res[f'checkpoint {cp}: vram.bin within fill'] = {
            'identical': vr['fill00-a'] == vr['fill00-b'] and vr['fillff-a'] == vr['fillff-b']}
    res['run-meta'] = {r: {k: m[k] for k in ('events', 'breakpoint_stops', 'vblanks', 'wall_seconds', 'rand_calls', 'stop_pc',
                                              'title_updates', 'checkpoints')}
                       for r, m in ((r, json.load(open(os.path.join(p, 'run-meta.json')))) for r, p in runs.items())}
    return res

RESOURCE_STATE_KEYS = ('handles', 'records', 'hash', 'live_handles', 'reserved', 'free_records', 'free_extras',
                       'descriptors', 'allocations', 'allocation_serials', 'free_queue_ids', 'inuse_ring_ids',
                       'reserved1', 'reserve_class', 'cache_switch', 'lower_switch', 'image_list_count', 'image_list',
                       'record_free_order', 'extra_free_order', 'handle_ids_free_order', 'handle_range_sha256',
                       'handle_nodes_sha256', 'material_tables', 'occupancy_hex', 'rand_state_214ec8_58')

def resource_equal(a, b, keys=RESOURCE_STATE_KEYS):
    out = {k: a.get(k) == b.get(k) for k in keys}
    for k in ('available', 'cursors', 'occupancy_sha256', 'nodes_sha256'):
        out['vram.' + k] = a['vram'][k] == b['vram'][k]
    return out

# ----------------------------------------------------------------------------------------------
def analyse_route(route, runs_dir, out, rng, title_runs=None, title_analysis=None):
    runs = {r: os.path.join(runs_dir, route, r) for r in RUNS}
    for r, p in runs.items():
        need(os.path.isdir(p), f'missing run {p}')
    os.makedirs(out)
    det = determinism(runs, ('title-constructed',) + CHECKPOINTS)
    need(all(v['identical'] for k, v in det.items() if isinstance(v, dict) and 'identical' in v),
         f'{route}: runs are not deterministic: ' + json.dumps({k: v.get('identical') for k, v in det.items() if isinstance(v, dict)}))
    a = runs['fill00-a']
    ev = read_jsonl(os.path.join(a, 'events.jsonl'))
    meta = json.load(open(os.path.join(a, 'run-meta.json')))
    need(meta.get('title_retire') == route and meta['route_mode'] == ROUTE_MODE[route] and meta['pad_step'] == 6,
         f'{route}: run-meta does not describe a completed {route} script')
    need(ev[-1]['ev'] == 'stop' and ev[-1]['kind'] == 'successor_constructor_entry' and ev[-1]['mode'] == ROUTE_MODE[route],
         f'{route}: capture does not end at the successor entry')
    tl, detail, counts = timeline(ev)
    for r in RUNS[1:]:
        need(timeline(read_jsonl(os.path.join(runs[r], 'events.jsonl')))[0] == tl, f'timeline differs in {r}')
    cps = checkpoint_boundaries(ev)
    need(tuple(k for k in cps if k != 'title-constructed') == CHECKPOINTS, f'{route}: checkpoints {list(cps)}')
    need(meta['checkpoints'] == ['title-constructed'] + list(CHECKPOINTS), f'{route}: checkpoint order {meta["checkpoints"]}')
    rng_rows = read_jsonl(rng)
    draws, draw_detail = rng_draws(ev, rng_rows)
    need(len(draws) >= len(rng_rows), 'fewer startup draws than RNG')

    # NIGHT_V table pointer (the 0x182e28 exit inside 0x12c350 selector 1)
    s1 = [e for e in ev if e['ev'] == 'enter' and e['name'] == 'mode8_construct' and e['selector'] == 1]
    need(len(s1) == 1, 'expected exactly one 0x12c350(selector 1)')
    s1x = [e for e in ev if e['ev'] == 'exit' and e['name'] == 'mode8_construct' and e['enter'] == s1[0]['i']][0]
    mt = [e for e in ev if e['ev'] == 'exit' and e['name'] == 'material_table' and s1[0]['i'] < e['i'] < s1x['i']]
    need(len(mt) == 1, 'expected one NIGHT_V material table')
    table_ptr = int(mt[0]['table']['ptr'], 16)

    write_x(os.path.join(out, 'events-detail.jsonl'), jl(detail))
    cp_info = {}
    for name in ('title-constructed',) + CHECKPOINTS:
        e, boundary = cps[name]
        fs = read_jsonl(os.path.join(a, 'checkpoints', name, 'state.jsonl'))[0]
        pre = prefix(detail, tl, boundary)
        resource = [x for x in pre if x['kind'] != 'phase']
        tu = [x['title_n'] for x in ev if x['ev'] == 'enter' and x.get('name') == 'title_update' and x['i'] <= e['i']]
        info = {'oracle_i': e['i'], 'boundary_oracle_i': boundary, 'vb': e['vb'], 'pc': e['pc'],
                'title_n': tu[-1] if tu else None, 'title_updates_entered': len(tu),
                'resource_events': len(resource),
                'events_by_kind': dict(sorted(collections.Counter(
                    x['kind'] if x['kind'] not in ('effect', 'mode') else x['kind'] + ':' + x['op'] for x in resource).items())),
                'rng_draws_before': sum(1 for d in draw_detail if d['oracle_i'] < boundary),
                'gs': e['gs'], 'stack_depth': e['stack_depth']}
        cp_info[name] = info
        if name == 'title-constructed':
            continue
        d = os.path.join(out, name)
        os.makedirs(d)
        write_x(os.path.join(d, 'events.jsonl'), jl(pre))
        st = native_state(fs, name, table_ptr)
        write_x(os.path.join(d, 'state.json'), json.dumps(st, separators=(',', ':')) + '\n')
        fx = {'effect106': effect_state(fs['title_effect']) if fs['title_effect_slot'] >= 0 else None}
        write_x(os.path.join(d, 'effects.json'), json.dumps(fx, separators=(',', ':')) + '\n')
        write_x(os.path.join(d, 'rng.jsonl'), jl(draws[:info['rng_draws_before']]))
        cpd = {r: os.path.join(runs[r], 'checkpoints', name) for r in RUNS}
        info['vram'] = vram_analysis(os.path.join(cpd['fill00-a'], 'vram.bin'), os.path.join(cpd['fillff-a'], 'vram.bin'),
                                     os.path.join(cpd['fill00-a'], 'vram-last-writer.bin'),
                                     os.path.join(cpd['fill00-a'], 'vram-upload-ever.bin'), d)
        info['state'] = {'allocations': len(st['allocations']), 'handles': len(st['handles']), 'records': len(st['records']),
                         'hash': len(st['hash']), 'image_list': st['image_list'], 'materials': len(st['materials']),
                         'available': st['available'], 'reserved': len(st['reserved'])}
        info['effect106'] = fx['effect106']
        info['effect_slots_live'] = [x['slot'] for x in fs['effect_slots']]
        info['theme_words_2d9a60'] = fs['theme_words_2d9a60']
    need(cp_info['first-update']['effect106'] is None, 'effect106 exists before the first title update')

    # ---- pad consumption, title frames ---------------------------------------------------------
    enters = {e['i']: e for e in ev if e['ev'] == 'enter'}
    pad_rows, frames = [], []
    cur = None
    for e in ev:
        if e['ev'] == 'enter' and e['name'] == 'mode8_update':
            cur = {'mode8_update_oracle_i': e['i'], 'vb': e['vb'], 'repeat_word': None, 'analog': None, 'pressed_word': None,
                   'title_n': None}
            pad_rows.append(cur)
        elif e['ev'] == 'exit' and e['name'] in ('pad_repeat', 'pad_pressed', 'pad_analog') and e['caller_ra'] in PAD_SITES:
            need(cur is not None and enters[e['enter']]['parent'] == 'mode8_update', 'pad read outside 0x12bfc8')
            k = PAD_SITES[e['caller_ra']]
            if k == 'analog':
                cur['analog'] = e['outputs']
            else:
                cur[k + '_word'] = e['word']
        elif e['ev'] == 'enter' and e['name'] == 'title_update':
            cur['title_n'] = e['title_n']
            fx = e['effect']
            frames.append({'title_n': e['title_n'], 'vb': e['vb'], 'oracle_i': e['i'], 'submode': e['submode'],
                           'counter': e['counter'], 'selection': e['selection'], 'pressed_word': e['pressed_word'],
                           'dir_flags': e['dir_flags'], 'dir_repeat': e['dir_repeat'], 'fade_b8': e['fade_b8'],
                           'effect106': effect_state(fx) if fx else None, 'effect_flags': fx['flags'] if fx and fx.get('flags') else None,
                           'effect_rgba_40': fx.get('rgba_40') if fx else None, 'host_buttons': e['host_buttons']})
            need(cur['pressed_word'] == e['pressed_word'], 'title pressed word differs from the 0x1c0160 read')
    write_x(os.path.join(out, 'pad-consumption.jsonl'), jl(pad_rows))
    write_x(os.path.join(out, 'title-frames.jsonl'), jl(frames))
    need([f['title_n'] for f in frames] == list(range(len(frames))), 'title update numbering')
    script = [{k: e[k] for k in ('i', 'vb', 'step', 'buttons', 'title_n', 'why')} for e in ev if e['ev'] == 'pad_script']
    host = read_jsonl(os.path.join(a, 'pad-host.jsonl'))
    nonzero = [r for r in pad_rows if r['repeat_word'] or r['pressed_word'] or (r['analog'] and r['analog'] != [128] * 4)]
    sub_changes = []
    for f in frames:
        if not sub_changes or sub_changes[-1]['submode'] != f['submode']:
            sub_changes.append({'submode': f['submode'], 'title_n': f['title_n'], 'vb': f['vb']})
    sel_changes = []
    for f in frames:
        if not sel_changes or sel_changes[-1]['selection'] != f['selection']:
            sel_changes.append({'selection': f['selection'], 'title_n': f['title_n'], 'vb': f['vb']})

    # ---- effect106 history, mode/retirement sequence -------------------------------------------
    fx_events = [d for d in detail if d['kind'] == 'effect']
    mode_events = [d for d in detail if d['kind'] == 'mode']
    flag_hist = []
    for e in ev:
        fx = e.get('effect') if e['ev'] in ('enter', 'exit') else None
        if fx and fx.get('flags') and (not flag_hist or flag_hist[-1]['flags'] != fx['flags']):
            flag_hist.append({'flags': fx['flags'], 'oracle_i': e['i'], 'vb': e['vb'], 'at': e['ev'] + ':' + e['name'],
                              'element_handles': fx['element_handles']})
    services = [e for e in ev if e['ev'] == 'enter' and e['name'] == 'effect_service']
    t_retire = cps['removal-request'][1]
    retire_seq = [{k: d[k] for k in ('kind', 'oracle_i', 'vb', 'caller_ra', 'parent') if k in d} | {
        x: d[x] for x in ('handle', 'alloc', 'delta', 'refs', 'op', 'node', 'selector', 'slot') if x in d}
                  for d in detail if d['oracle_i'] >= t_retire and d['kind'] != 'phase']
    rr = [e for e in ev if e['ev'] == 'enter' and e['name'] == 'resource_retire' and e.get('mode8_selector') == 1]
    need(len(rr) == 1, 'title resource child retired exactly once')
    rr_exit = [e for e in ev if e['ev'] == 'exit' and e['name'] == 'resource_retire' and e['enter'] == rr[0]['i']][0]
    in_rr = [d for d in detail if rr[0]['i'] < d['oracle_i'] < rr_exit['i']]
    after_rr = [d for d in detail if d['oracle_i'] > rr_exit['i'] and d['kind'] not in ('phase', 'effect', 'mode')]
    need(not after_rr, 'texture resource events after the title resource child retirement: ' + json.dumps(after_rr[:3]))
    retire_callers = collections.Counter((d['kind'], d['caller_ra']) for d in in_rr if d['kind'] == 'retire')
    mtr = [e for e in ev if e['ev'] == 'enter' and e['name'] == 'model_table_retire']
    need(len(mtr) == 1 and int(mtr[0]['table']['ptr'], 16) == table_ptr, 'NIGHT_V table retirement not observed once')
    model_retire_order = [d['handle'] for d in in_rr if d['kind'] == 'retire' and d['caller_ra'] == '0x1796f8']
    image_retire_order = [d['handle'] for d in in_rr if d['kind'] == 'retire' and d['caller_ra'] != '0x1796f8']
    host_frees = [{'oracle_i': e['i'], 'vb': e['vb'], 'ra': e['ra'], 'parent': e['parent'], 'ptr': e['ptr']}
                  for e in ev if e['ev'] == 'enter' and e['name'] == 'host_free' and rr[0]['i'] < e['i']]

    # ---- survivors and pending tails at the stop ------------------------------------------------
    fs_stop = read_jsonl(os.path.join(a, 'checkpoints', 'successor-entry', 'state.jsonl'))[0]
    fs_prefix = read_jsonl(os.path.join(a, 'state-at-100618.jsonl'))[0]
    succ = [e for e in ev if e['ev'] == 'enter' and e['name'] == 'successor_construct'][0]
    survivors = {
        'equal_to_state_at_0x100618': {k: fs_stop[k] == fs_prefix[k] for k in ('handles', 'records', 'hash', 'live_handles',
                                                                                'reserved', 'free_records', 'free_extras')},
        'live_allocations': sum(1 for x in fs_stop['allocations'] if x['live']),
        'live_allocation_ids_equal_prefix': [x['id'] for x in fs_stop['allocations'] if x['live']]
        == [x['id'] for x in fs_prefix['allocations'] if x['live']],
        'descriptor_refs_equal_prefix': fs_stop['descriptors'] == fs_prefix['descriptors'],
        'available': fs_stop['vram']['available'], 'available_at_prefix': fs_prefix['vram']['available'],
        'cursors_equal_prefix': fs_stop['vram']['cursors'] == fs_prefix['vram']['cursors'],
        'occupancy_equal_prefix': fs_stop['vram']['occupancy_sha256'] == fs_prefix['vram']['occupancy_sha256'],
        'free_queue_ids_head': fs_stop['free_queue_ids'][:12], 'image_list': fs_stop['image_list'],
        'material_tables': [{'ptr': t['ptr'], 'count': t['count']} for t in fs_stop['material_tables']],
    }
    pending = {
        'effect106': effect_state(fs_stop['title_effect']),
        'effect106_job': fs_stop['title_effect'],
        'effect106_note': 'flags 0x200 = reclaim pending: 0x102218 frees the job on its next service; the title resource '
                          'child that ran 0x102218 (0x100f90) is retired, so no service has run since; the job still names '
                          'the retired hanten handle',
        'effect_slots_live': [x['slot'] for x in fs_stop['effect_slots']],
        'theme_words_2d9a60': fs_stop['theme_words_2d9a60'],
        'theme_note': '0x12dd50(1) at 0x12c8d8 set [request 1, stream 1, countdown 0x80]; the countdown has advanced to the '
                      'value shown; stream destruction has not happened (deferred audio tail)',
        'host_buttons_at_stop': frames[-1]['host_buttons'] if frames else None,
        'successor_entry': {k: succ[k] for k in ('node', 'parent', 'parent_40_mode', 'parent_44', 'parent_4c', 'mode_24f738',
                                                 'descriptor_ctor')},
    }

    oq = open_questions(ev, frames, cps, fs_stop['title_effect_slot'])
    oq['O08']['theme_words_at_stop'] = fs_stop['theme_words_2d9a60']
    ref = {
        'open_questions': oq,
        'schema': 'okage.retire.title-original-reference.v1', 'route': route, 'route_mode': ROUTE_MODE[route],
        'checkpoint_rule': 'a checkpoint at a function entry precedes every event of that call (boundary = entry index, '
                           'exclusive); title-constructed is the Title stop (0x12c350 selector-1 return)',
        'checkpoints': cp_info,
        'timeline_counts': dict(sorted(counts.items())),
        'pad': {'script_actions': script, 'host_pad_changes': host, 'mode8_pad_reads': len(pad_rows),
                'nonneutral_reads': nonzero, 'confirm_mask_1f85dc': '0x40',
                'pad_mask_table_1f85c8': [e for e in ev if e['ev'] == 'enter' and e['name'] == 'title_update'][0]['pad_mask_table_1f85c8']},
        'title': {'updates': len(frames), 'submode_changes': sub_changes, 'selection_changes': sel_changes,
                  'first_accepting_title_n': next(f['title_n'] for f in frames if f['submode'] == 10)},
        'effect106': {'events': fx_events, 'flag_history': flag_hist, 'services_0x102218': len(services),
                      'last_service_vb': services[-1]['vb'] if services else None},
        'mode_events': mode_events,
        'retirement': {'title_resource_child_enter_i': rr[0]['i'], 'exit_i': rr_exit['i'], 'vb': rr[0]['vb'],
                       'retire_callers': {f'{k[0]}@{k[1]}': v for k, v in retire_callers.items()},
                       'model_table_retire_order': model_retire_order, 'image_list_retire_order': image_retire_order,
                       'nightv_table_ptr': hex(table_ptr), 'sequence_from_removal_request': retire_seq,
                       'host_frees_from_resource_retire': host_frees,
                       'texture_events_after_resource_child_return': len(after_rr)},
        'rng': {'draws_total': len(draws), 'startup_draws_rng': len(rng_rows), 'draws_after_prefix':
                [d for d in draw_detail if d['after_prefix']], 'state_at_stop': ev[-1]['rand_state_214ec8_58']},
        'survivors_at_successor_entry': survivors, 'pending_at_stop': pending,
        'determinism': det,
    }
    with open(os.path.join(out, 'rng-detail.jsonl'), 'x') as f:
        f.write(jl(draw_detail[len(rng_rows):]))

    # ---- control: minimal hooks (title update entry + successor entry only) -------------------------
    ctl = os.path.join(runs_dir, route, CONTROL)
    if os.path.isdir(ctl):
        cev = read_jsonl(os.path.join(ctl, 'events.jsonl'))
        cframes = [{k: e[k] for k in ('title_n', 'submode', 'counter', 'selection', 'pressed_word', 'dir_flags', 'dir_repeat',
                                      'fade_b8', 'host_buttons')}
                   for e in cev if e['ev'] == 'enter' and e['name'] == 'title_update']
        hframes = [{k: e[k] for k in ('title_n', 'submode', 'counter', 'selection', 'pressed_word', 'dir_flags', 'dir_repeat',
                                      'fade_b8', 'host_buttons')}
                   for e in ev if e['ev'] == 'enter' and e['name'] == 'title_update']
        cmeta = json.load(open(os.path.join(ctl, 'run-meta.json')))
        cmp_cps = {}
        for name in ('first-update', 'stable-menu', 'successor-entry'):
            cs = read_jsonl(os.path.join(ctl, 'checkpoints', name, 'state.jsonl'))[0]
            hs = read_jsonl(os.path.join(a, 'checkpoints', name, 'state.jsonl'))[0]
            # allocations/allocation_serials are reconstructed by the observer from 0x1cf008 hooks (absent in the
            # minimal control); every other key is read from guest RAM.
            cmp_cps[name] = {'resource_state_equal': resource_equal(cs, hs, tuple(k for k in RESOURCE_STATE_KEYS
                                                                             if k not in ('allocations', 'allocation_serials'))),
                             'title_n_equal': [[x['title_n'] for x in cev if x['ev'] == 'enter' and x.get('name') == 'title_update'
                                                and x['i'] <= e['i']][-1] for e in cev if e['ev'] == 'checkpoint' and e['name'] == name]
                             == [cp_info[name]['title_n']],
                             'vram_fill00_equal': fsha(os.path.join(ctl, 'checkpoints', name, 'vram.bin'))
                             == fsha(os.path.join(a, 'checkpoints', name, 'vram.bin')),
                             'vb_control': [e['vb'] for e in cev if e['ev'] == 'checkpoint' and e['name'] == name][0],
                             'vb_hooked': cp_info[name]['vb']}
        ref['control_minimal'] = {
            'vblanks': cmeta['vblanks'], 'title_updates': cmeta['title_updates'], 'checkpoints': cmeta['checkpoints'],
            'title_frames_equal': cframes == hframes, 'pad_host_equal_modulo_vb':
                [{k: r[k] for k in ('buttons', 'play_mask')} for r in read_jsonl(os.path.join(ctl, 'pad-host.jsonl'))]
                == [{k: r[k] for k in ('buttons', 'play_mask')} for r in host],
            'checkpoints_compared': cmp_cps,
            'final_vram_equal': fsha(os.path.join(ctl, 'vram.bin')) == fsha(os.path.join(a, 'vram.bin')),
        }

    # ---- hook neutrality at the Title stop ----------------------------------------------------------
    if title_runs and title_analysis:
        title_fs = read_jsonl(os.path.join(title_runs, 'fill00-a', 'final-state.jsonl'))[0]
        tc = read_jsonl(os.path.join(a, 'checkpoints', 'title-constructed', 'state.jsonl'))[0]
        title_tl = read_jsonl(os.path.join(title_analysis, 'events.jsonl'))
        e, boundary = cps['title-constructed']
        mine = [x for x in prefix(detail, tl, boundary + 1) if x['kind'] not in ('phase', 'effect', 'mode')]
        cd = os.path.join(a, 'checkpoints', 'title-constructed')
        ref['title_neutrality'] = {
            'state_equal_to_title_fill00_a_final_state': resource_equal(tc, title_fs),
            'pc_equal': tc['pc'] == title_fs['pc'],
            'vram_fill00_equal': fsha(os.path.join(cd, 'vram.bin')) == fsha(os.path.join(title_runs, 'fill00-a', 'vram.bin')),
            'vram_fillff_equal': fsha(os.path.join(runs['fillff-a'], 'checkpoints', 'title-constructed', 'vram.bin'))
            == fsha(os.path.join(title_runs, 'fillff-a', 'vram.bin')),
            'last_writer_equal': fsha(os.path.join(cd, 'vram-last-writer.bin'))
            == fsha(os.path.join(title_runs, 'fill00-a', 'vram-last-writer.bin')),
            'upload_ever_equal': fsha(os.path.join(cd, 'vram-upload-ever.bin'))
            == fsha(os.path.join(title_runs, 'fill00-a', 'vram-upload-ever.bin')),
            'texture_timeline_equal_to_title_analysis': mine == [x for x in title_tl if x['kind'] != 'phase'],
            'texture_events': len(mine),
            'vb_title': json.load(open(os.path.join(title_runs, 'fill00-a', 'run-meta.json')))['vblanks'],
            'vb_retire': e['vb'],
        }
        bad = [k for k, v in ref['title_neutrality'].items() if v is False] + [
            k for k, v in ref['title_neutrality']['state_equal_to_title_fill00_a_final_state'].items() if not v]
        ref['title_neutrality']['failures'] = bad
    with open(os.path.join(out, 'reference.json'), 'x') as f:
        json.dump(ref, f, indent=1)
        f.write('\n')
    return ref

# ----------------------------------------------------------------------------------------------
def open_questions(ev, frames, cps, slot):
    """Answers to the static contract's open questions O01..O12 (the title-retirement input contract), from this capture."""
    s1 = next(e['i'] for e in ev if e['ev'] == 'enter' and e.get('name') == 'mode8_construct' and e.get('selector') == 1)
    first_title = next(e['i'] for e in ev if e['ev'] == 'enter' and e.get('name') == 'title_update')
    succ_i = cps['successor-entry'][1]
    ent = lambda n, lo=s1: [e for e in ev if e['ev'] == 'enter' and e.get('name') == n and e['i'] >= lo]  # noqa: E731
    exits = {e['enter']: e for e in ev if e['ev'] == 'exit'}
    q = {}
    rets = ent('handle_retire', first_title)
    q['O01'] = {'question': '0x1cca30 a0, s3 (record) after 0x1cca7c, hash slot written at 0x1ccad4',
                'retires': [{'handle': e['handle_or_record'], 'record': e['record'], 'handles_before': e['record_handles_before'],
                             'hash_slot_written_minus1': e['record_hash_slot'] if e['record_handles_before'] == 1 and e['record_hash_slot'] >= 0 else None,
                             'caller_ra': e['ra']} for e in rets],
                'note': 's3 = s16 0x3a5b00[a0] (logged as record at entry); 0x1ccad4 stores -1 into hash[record+0x24] when the '
                        'record count reaches 0 and the slot is >= 0 (tombstone, the hash keeps a -1 entry)'}
    frees = ent('free', first_title)
    q['O02'] = {'question': '0x1cf728 a0 (u64 descriptor), 0x1cf770 v0 (0x1cede8 refs), allocator dump at 0x124548',
                'calls': [{'desc': e['args'][0], 'freed': exits[e['i']]['freed']} for e in frees],
                'refs_query_results': dict(collections.Counter(exits[e['i']]['result'] for e in ent('desc_refs_query', first_title))),
                'freed': sum(1 for e in frees if exits[e['i']]['freed']),
                'allocator_dump': 'checkpoints/successor-entry/state.jsonl (descriptors, free queue, cursors, occupancy)'}
    al = [x for x in (exits.get(e['i']) for e in ent('effect_alloc')) if x]
    tb = ent('effect_template_bind', first_title)
    q['O03'] = {'question': '0x1294d4 v0, job +0x50..+0x5f/+0x68, 0x10c248 s1, 0x10c7b8 a2',
                'effect_alloc_returns': [{k: x.get(k) for k in ('slot', 'job_50_5f', 'job_68', 'element0_7c', 'vb')} for x in al],
                'bind_element_7c_at_entry': [e.get('element_7c') for e in ent('effect_bind')],
                'template_bind_a2_values': sorted(set(e['a2'] for e in tb)), 'template_bind_calls': len(tb),
                'template_bind_callers': sorted(set(e['ra'] for e in tb)),
                'note': '0x10c248 tests s1 = element +0x7c (loaded at 0x10c19x and not written before 0x10c248); 0 means 0x10c188 '
                        'returns after the descriptor/size writes without the 0x2020 grid fill or 0x10bec0'}
    q['O04'] = {'question': 'rand() entries between the first 0x1296f8 and 0x124548',
                'rand_entries': [e for e in ev if e['ev'] == 'rand' and first_title <= e['i'] < succ_i]}
    ic = [exits[e['i']] for e in ent('title_instance_create') if e['i'] in exits]
    q['O05'] = {'question': '0x1b8d74 v0 (0x1b8a80 return), instance +0/+2/+0x30, 0x1b6f34 a0, 0x179810',
                'title_instance_create': [{'index': x['index'], 'instance': x['instance'], 'vb': x['vb']} for x in ic],
                'instance_retire_nonempty': [{'index': e['index'], 'instance': e['instance'], 'vb': e['vb']}
                                             for e in ent('instance_retire') if e['instance'] and e['instance']['h0']],
                'instance_release_a0': [e['index'] for e in ent('instance_release')],
                'model_group_retire_a0': [e['value'] for e in ent('model_group_retire')],
                'model_table_retire_holder': [e['holder'] for e in ent('model_table_retire')]}
    q['O06'] = {'question': '0x1b84c0 v0 (*0x337ef0), 0x1b5fe0 v0 (*0x337610), 0x1b6018 v0 (*0x337920)',
                'w_337ef0_at_0x1b84b0': [e['w_337ef0'] for e in ent('scene_retire')],
                'at_0x1b5fd0': [{'slots_337610': e['slots_337610'], 'count_337920': e['count_337920']} for e in ent('image_slots_retire')],
                'at_0x1b4b00': [{'w_337610': e['w_337610'], 'w_337920': e['w_337920']} for e in ent('image_list_release_tail')],
                'image_slot_release_calls': len(ent('image_slot_release'))}
    per_sub = collections.Counter(f['submode'] for f in frames)
    rr = cps['removal-request'][0]
    last = [f for f in frames if f['oracle_i'] < rr['i']][-1]
    q['O07'] = {'question': 'mode8 +0x2c/+0x30/+0x38/+0xb8 at each 0x1296f8 entry; frame reaching 0x12a26c',
                'per_title_update': 'title-frames.jsonl', 'updates_per_submode': {str(k): v for k, v in sorted(per_sub.items())},
                'removal_request_title_n': last['title_n'], 'fade_b8_at_that_entry': last['fade_b8'],
                'removal_request_vb': rr['vb']}
    q['O08'] = {'question': '0x102440 water reclaim / 0x12ce8c theme end pending at the stop',
                'water_job_freed_before_stop': any(e.get('job_slot') == slot for e in ent('effect_free', first_title)),
                'theme_words_at_stop': None}
    se = ent('successor_construct')
    q['O09'] = {'question': '*0x1f85c0+0x18 children at 0x124548', 'root': se[0].get('root') if se else None,
                'root_children': se[0].get('root_children') if se else None,
                'scheduler_free_sites': sorted(set(e['ra'] for e in ent('host_free', 0) if 'scheduler_node' in e))}
    q['O10'] = {'question': '0x1976f0 glyph-cache transfers', 'calls_whole_run': len(ent('glyph_cache_transfer', 0))}
    probes = collections.Counter((e['name'], e.get('target_job_68') or e.get('target_213574') or e.get('target_213580')
                                  or e.get('target_21357c')) for e in ev if e['ev'] == 'probe')
    q['O11'] = {'question': 'jalr targets at 0x1024c8, 0x103ba0, 0x103c20, 0x103c68 (and 0x102e44)',
                'probe_hits_by_target': [{'probe': k[0], 'target_source_value': k[1], 'hits': v} for k, v in sorted(probes.items(), key=str)],
                'note': 'job +0x68 == 0 means the jalr is skipped (beqz); 0x1024d0 not hit means the 0x1024bc path was not reached'}
    rend = [e for e in ev if e['ev'] == 'probe' and e['name'] == 'effect_render_callback_return' and e.get('slot') == slot
            and e['i'] > s1]
    q['O12'] = {'question': 'job +0x40..+0x4c at 0x102e4c on the first water render',
                'first_renders': [{k: e.get(k) for k in ('vb', 'rgba_40', 'flags', 'target_job_68')} for e in rend[:3]]}
    return q

def regression(title_run, default_run, title_runs, startup_run):
    out = {}
    if title_run:
        ref = os.path.join(title_runs, 'fill00-a')
        r = {}
        for n in ('events.jsonl', 'disc-opens.jsonl', 'vram.bin', 'vram-last-writer.bin', 'vram-upload-ever.bin',
                  'ee-ram-32mb.bin', 'ee-ram-after-10bc78.bin', 'final-state.jsonl', 'state-at-100618.jsonl'):
            r[n] = fsha(os.path.join(title_run, n)) == fsha(os.path.join(ref, n))
        r['gs-ledger.jsonl (ee_event removed)'] = title.ledger_without_ee_cross_reference(os.path.join(title_run, 'gs-ledger.jsonl')) \
            == title.ledger_without_ee_cross_reference(os.path.join(ref, 'gs-ledger.jsonl'))
        ma, mb = (json.load(open(os.path.join(p, 'run-meta.json'))) for p in (title_run, ref))
        ma.pop('wall_seconds')
        mb.pop('wall_seconds')
        r['run-meta.json (wall_seconds removed)'] = ma == mb
        out['title_mode_vs_title_fill00_a'] = r
    if default_run:
        r = {}
        for n in ('events.jsonl', 'disc-opens.jsonl', 'gs-ledger.jsonl', 'vram.bin', 'ee-ram-32mb.bin', 'final-state.jsonl'):
            r[n] = fsha(os.path.join(default_run, n)) == fsha(os.path.join(startup_run, n))
        ma, mb = (json.load(open(os.path.join(p, 'run-meta.json'))) for p in (default_run, startup_run))
        common = sorted((set(ma) & set(mb)) - {'wall_seconds'})
        r['run-meta.json (keys common to both, wall_seconds removed)'] = {k: ma[k] for k in common} == {k: mb[k] for k in common}
        out['default_mode_run_meta_keys_added_since_startup_binary'] = {k: ma[k] for k in sorted(set(ma) - set(mb))}
        out['default_mode_vs_startup_fill00_a'] = r
    return out

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('evidence')
    p.add_argument('--out', required=True)
    p.add_argument('--rng', required=True)
    p.add_argument('--title-runs')
    p.add_argument('--title-analysis')
    p.add_argument('--startup-run')
    p.add_argument('--regress-title')
    p.add_argument('--regress-default')
    a = p.parse_args(argv)
    if os.path.exists(a.out):
        print('refusing existing output directory', file=sys.stderr)
        return 2
    try:
        os.makedirs(a.out)
        refs = {}
        for route in ROUTES:
            refs[route] = analyse_route(route, os.path.join(a.evidence, 'runs'), os.path.join(a.out, route), a.rng,
                                        a.title_runs, a.title_analysis)
        if a.regress_title or a.regress_default:
            need(a.title_runs and (a.startup_run or not a.regress_default), '--regress-* needs --title-runs/--startup-run')
            reg = regression(a.regress_title, a.regress_default, a.title_runs, a.startup_run)
            with open(os.path.join(a.out, 'regression.json'), 'x') as f:
                json.dump(reg, f, indent=1)
                f.write('\n')
            for k, v in reg.items():
                if k.endswith('fill00_a'):
                    print(k, 'all identical' if all(v.values()) else 'DIFFERENT: ' + json.dumps(v))
                else:
                    print(k, json.dumps(v))
    except Fail as exc:
        print('FAIL:', exc, file=sys.stderr)
        return 1
    for route, ref in refs.items():
        print(f'== {route}')
        for name, c in ref['checkpoints'].items():
            line = f'  {name}: vb={c["vb"]} title_n={c["title_n"]} resource_events={c["resource_events"]}'
            if 'state' in c:
                line += ' allocations=%d handles=%d image_list=%s materials=%d effect106=%s vram_known=%d vram_unknown=%d' % (
                    c['state']['allocations'], c['state']['handles'], c['state']['image_list'], c['state']['materials'],
                    json.dumps(c['effect106']), c['vram']['known_nibbles_required'], c['vram']['unknown_nibbles_required'])
            print(line)
        if 'title_neutrality' in ref:
            print('  title neutrality failures:', ref['title_neutrality']['failures'])
        if 'control_minimal' in ref:
            cm = ref['control_minimal']
            print('  control: title_frames_equal=%s pad_equal=%s vblanks=%d' % (cm['title_frames_equal'], cm['pad_host_equal_modulo_vb'],
                                                                                cm['vblanks']))
    return 0

if __name__ == '__main__':
    sys.exit(main())
