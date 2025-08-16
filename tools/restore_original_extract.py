#!/usr/bin/env python3
"""Extract the Restore-cancel original reference from `okage-startup-oracle --restore-cancel` captures.

usage: restore_original_extract.py EVIDENCE_DIR --out ANALYSIS_DIR --rng RNG_JSONL
           --retire-evidence RETIRE_EVIDENCE_DIR [--regress-retire RUN] [--regress-title RUN] [--regress-default RUN]
           [--title-runs DIR] [--startup-run DIR]

EVIDENCE_DIR/runs/{fill00-a,fill00-b,fillff-a,fillff-b,control-minimal-fill00} (tools/restore_capture.sh).
--retire-evidence: the pinned Retire title-retirement evidence (runs/restore/* and analysis/restore/*); the Restore run must
reproduce it exactly up to the 0x124548 entry (retire-successor-entry).
--regress-retire/--regress-title/--regress-default: runs of the SAME Restore oracle binary in `--title-retire restore`,
`--title` and default mode, checked byte-identical (modulo documented host-timing fields) to the Retire restore
fill00-a, Title fill00-a and Startup fill00-a captures.

Writes ANALYSIS_DIR (must not exist):
  reference.json                 checkpoints (vblank, traversal, boundary), pad script/consumption, state machine,
                                 manager frames, RNG per phase/site/traversal, atlas and second-title ownership,
                                 tails, GS writes, glyph verdict, O01..O15 of the Restore input contract,
                                 determinism, Retire neutrality, minimal control, regression
  events-detail.jsonl            the ordered lifecycle timeline with source attribution (oracle index, vblank,
                                 traversal, caller)
  manager-frames.jsonl           one row per Restore traversal (t = 0 is the construction traversal S)
  pad-consumption.jsonl          every pad read inside 0x11e480
  rng-detail.jsonl               every draw after the 0x100618 prefix (ra, frames, state before/after, traversal)
  gs-writes.jsonl                every GS host-to-local transfer after the Retire successor entry (ledger rows)
  <checkpoint>/events.jsonl      lifecycle prefix from cold boot (native lifecycle schema, Restore vocabulary)
  <checkpoint>/state.json        allocator/handle/record/list state (coverage lifecycle "restore")
  <checkpoint>/effects.json      {"effect106": ...}
  <checkpoint>/rng.jsonl         every draw before the checkpoint
  <checkpoint>/vram-written-mask.bin, vram-written-values.bin, vram-invalidated-mask.bin
Reads only; exits 1 if the runs disagree or a cross-check fails. Research evidence only; never product input.
"""
import argparse
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import title_original_extract as title  # noqa: E402
import retire_title_original_extract as retire  # noqa: E402

RUNS = ('fill00-a', 'fill00-b', 'fillff-a', 'fillff-b')
CONTROL = 'control-minimal-fill00'
RETIRE_CPS = ('retire-title-constructed', 'retire-first-update', 'retire-stable-menu', 'retire-removal-request',
           'retire-resource-retirement')
CHECKPOINTS = ('retire-successor-entry', 'restore-constructor-return', 'first-layout', 'empty-ready', 'cancel-death',
               'resource-retirement', 'wrapper-free', 'second-title-return')
ENTRY_TRIGGER = {'retire-title-constructed': None, 'retire-first-update': 'title_update', 'retire-stable-menu': 'title_update',
                 'retire-removal-request': 'effect_remove_request', 'retire-resource-retirement': 'mode8_destroy',
                 'retire-successor-entry': 'successor_construct', 'empty-ready': 'restore_input',
                 'cancel-death': 'manager_destroy', 'resource-retirement': 'wrapper_destroy', 'wrapper-free': 'host_free'}
RETURN_TRIGGER = {'restore-constructor-return': 'successor_construct', 'first-layout': 'resource_tick',
                  'second-title-return': 'mode8_construct'}
MEMORY = 'host-uploads-known-draw-writes-unknown'
SCHEDULER_PC = '0x100618'
RNG_SITES = {'0x19c754': 'restore-ui-glyph', '0x19cadc': 'restore-ui-icon', '0x19cc80': 'restore-ui-select'}

Fail = title.Fail
need = title.need
sha = title.sha
fsha = title.fsha
read_jsonl = title.read_jsonl
jl = title.jl
write_x = retire.write_x
effect_state = retire.effect_state

def timeline(events, light=False):
    enters = {}
    out, detail = [], []
    live = set()
    archive = xmd_path = None
    counts = collections.Counter()
    title_n = None
    last_sub = None
    trav = 0
    ui_state = None

    def add(row, e, ent=None):
        out.append(row)
        d = dict(row)
        src = ent or e
        d.update({'oracle_i': e['i'], 'vb': e.get('vb'), 'trav': trav, 'top': e.get('top'), 'fn': src.get('fn'),
                  'caller_ra': src.get('ra'), 'parent': src.get('parent'), 'title_n': title_n})
        detail.append(d)
        counts[row['kind'] if row['kind'] not in ('effect', 'mode', 'ui', 'glyph') else row['kind'] + ':' + row['op']] += 1

    def owner_row(e, op):
        o = e['owner']
        node = {'mode9_10': 'mode10'}.get(o['node_kind'], o['node_kind'])
        need(o['owner_mode'] in (8, 10) and o['owner_selector'] >= 0, f'unmodelled owner {o}')
        if op == 'wrapper-free':
            need(node in ('resource', 'mode8', 'mode10', 'manager'), f'scheduler freed an unmodelled node {o}')
            return {'kind': 'mode', 'op': op, 'node': node, 'mode': o['owner_mode'], 'selector': o['owner_selector']}
        return {'kind': 'mode', 'op': op, 'mode': o['owner_mode'], 'selector': o['owner_selector']}

    for e in events:
        ev = e['ev']
        if ev == 'top' and e['pc'] == SCHEDULER_PC:
            trav += 1
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
                if 'owner' in e:
                    add(owner_row(e, 'resource-retire'), e)
                else:
                    need(e.get('mode8_selector', -1) >= 0, 'resource child retired without a mode-8 parent')
                    add({'kind': 'mode', 'op': 'resource-retire', 'mode': 8, 'selector': e['mode8_selector']}, e)
            elif n == 'mode8_destroy':
                add({'kind': 'mode', 'op': 'destroy', 'mode': 8, 'selector': e['selector']}, e)
            elif n == 'wrapper_destroy':
                w = e['wrapper']
                add({'kind': 'mode', 'op': 'destroy', 'mode': w['w24_mode'], 'selector': w['w28_selector']}, e)
            elif n == 'mode_request':
                add({'kind': 'mode', 'op': 'request', 'mode': e['mode'], 'selector': e['selector'], 'arg': e['arg3']}, e)
            elif n == 'host_free' and 'scheduler_node' in e:
                if 'owner' in e:
                    add(owner_row(e, 'wrapper-free'), e)
                else:
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
            elif n == 'restore_input' and light:
                # light window: 0x11c2c8 is not hooked; a state it set is first seen at the next 0x11e480 entry (no
                # lifecycle row can occur between that return and this entry)
                s = e['restore']['state_1f89a8']
                if ui_state is None:
                    ui_state = s
                elif s != ui_state:
                    add({'kind': 'ui', 'op': 'state', 'state': s}, e)
                    ui_state = s
            elif n == 'window_destroy':
                add({'kind': 'ui', 'op': 'window-death', 'window': e['value']}, e)
            elif n == 'manager_destroy':
                add({'kind': 'ui', 'op': 'manager-destroy', 'mode': 10}, e)
            elif n == 'glyph_cache_transfer':
                add({'kind': 'glyph', 'op': 'cache-transfer'}, e)
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
            elif n == 'upload_qwc' and 'upload_sha256' in e:
                need(e['upload_sha256'] is not None, 'upload bytes not hashable')
                add({'kind': 'upload-bytes', 'alloc': e['upload_desc']['id'], 'bytes': e['upload_bytes'],
                     'sha256': e['upload_sha256']}, e, ent)
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
            elif n == 'manager_construct':
                add({'kind': 'ui', 'op': 'manager-create', 'mode': 10, 'windows': e['windows']}, e, ent)
                for h in e['window_handles']:
                    add({'kind': 'ui', 'op': 'window-create', 'window': h}, e, ent)
            elif n in ('restore_input', 'restore_update'):
                s = e['restore']['state_1f89a8']
                if s != ui_state:
                    add({'kind': 'ui', 'op': 'state', 'state': s}, e, ent)
                    ui_state = s
        elif ev == 'prefix_end':
            add({'kind': 'phase', 'name': 'prefix-end-0x100618'}, e)
        elif ev == 'checkpoint':
            add({'kind': 'phase', 'name': 'checkpoint-' + e['name']}, e)
        elif ev == 'stop':
            add({'kind': 'phase', 'name': 'stop-' + e['kind']}, e)
    need(all(a['oracle_i'] <= b['oracle_i'] for a, b in zip(detail, detail[1:])), 'timeline not in source order')
    return out, detail, counts

RETIRE_KINDS = ('alloc', 'upload', 'ref', 'free', 'handle', 'retire', 'bind', 'name', 'reserve', 'material', 'phase',
             'effect', 'mode')

def retire_view(rows):
    """The Retire vocabulary subset of a Restore lifecycle (drops upload-bytes/ui/glyph rows)."""
    return [r for r in rows if r['kind'] in RETIRE_KINDS]

def checkpoint_boundaries(ev):
    out = {}
    by_i = {e['i']: e for e in ev}
    for e in ev:
        if e['ev'] != 'checkpoint':
            continue
        name = e['name']
        need(name not in out, 'checkpoint repeated: ' + name)
        prev = by_i[e['i'] - 1]
        if name == 'retire-title-constructed':
            out[name] = (e, e['i'])
        elif name in RETURN_TRIGGER:
            need(prev['ev'] == 'exit' and prev['name'] == RETURN_TRIGGER[name],
                 f'checkpoint {name} is not directly after its {RETURN_TRIGGER[name]} return')
            out[name] = (e, prev['i'] + 1)
        else:
            need(name in ENTRY_TRIGGER, 'unknown checkpoint ' + name)
            need(prev['ev'] == 'enter' and prev['name'] == ENTRY_TRIGGER[name] and prev['fn'] == e['pc'],
                 f'checkpoint {name} is not directly after its {ENTRY_TRIGGER[name]} entry')
            out[name] = (e, prev['i'])
    return out

def prefix(rows_detail, rows, boundary):
    k = 0
    while k < len(rows_detail) and rows_detail[k]['oracle_i'] < boundary:
        k += 1
    return rows[:k]

def rng_draws(ev, rng, successor_i):
    """Retire draw extraction; draws after the Retire successor entry are labelled by their call site."""
    draws, detail = retire.rng_draws(ev, rng)
    trav = 0
    tr_at = {}
    for e in ev:
        if e['ev'] == 'top' and e['pc'] == SCHEDULER_PC:
            trav += 1
        if e['ev'] == 'rand':
            tr_at[e['i']] = trav
    for d, x in zip(draws, detail):
        x['trav'] = tr_at[x['oracle_i']]
        if x['oracle_i'] > successor_i:
            need(x['ra'] in RNG_SITES, f'draw at an unmodelled site {x["ra"]}')
            d['phase'] = RNG_SITES[x['ra']]
            x['phase'] = d['phase']
    return draws, detail

def table_ptrs(ev):
    """NIGHT_V material table pointers created inside each 0x12c350(selector 1)."""
    out = []
    for s in (e for e in ev if e['ev'] == 'enter' and e['name'] == 'mode8_construct' and e['selector'] == 1):
        x = next(e for e in ev if e['ev'] == 'exit' and e['name'] == 'mode8_construct' and e['enter'] == s['i'])
        mt = [e for e in ev if e['ev'] == 'exit' and e['name'] == 'material_table' and s['i'] < e['i'] < x['i']]
        need(len(mt) == 1, 'expected one NIGHT_V material table per selector-1 title')
        out.append(int(mt[0]['table']['ptr'], 16))
    return out

def native_state(fs, cp, ptrs):
    st = retire.native_state(fs, cp, -1)
    tables = [t for t in fs['material_tables'] if t and int(t['ptr'], 16) in ptrs]
    need(len(tables) <= 1, 'more than one NIGHT_V material table listed')
    st['materials'] = [[i, h] for i, h in enumerate(tables[0]['handles'])] if tables else []
    st['coverage'] = {'lifecycle': 'restore', 'checkpoint': cp, 'memory': MEMORY}
    return st

# ----------------------------------------------------------------------------------------------
def in_input(ra):
    """Pad reads are attributed to 0x11e480 by their call site (0x11e480..0x11f5f8), not by the observer's frame
    stack, which interleaves the card thread."""
    return ra is not None and 0x11e480 <= int(ra, 16) < 0x11f5f8

def manager_frames(ev, s_trav, draw_detail, successor_i):
    """One row per traversal from the construction traversal S (t = 0) through the death traversal D, holding the
    0x11e480 (input) and 0x11c2c8 (update) observations of that traversal and its RNG draw count."""
    enters = {e['i']: e for e in ev if e['ev'] == 'enter'}
    rows = {}
    trav = 0
    cur_input = None
    for e in ev:
        if e['ev'] == 'top' and e['pc'] == SCHEDULER_PC:
            trav += 1
        if e['i'] <= successor_i:
            continue
        if e['ev'] == 'enter' and e['name'] == 'restore_input':
            t = trav - s_trav
            need(t not in rows or 'input_state_in' not in rows[t], f'two 0x11e480 calls in traversal {t}')
            r = rows.setdefault(t, {'t': t})
            r.update({'vb': e['vb'], 'input_state_in': e['restore']['state_1f89a8'], 'gate_open': e['input_gate_open'],
                      'opacity_bits': (e['restore'].get('ui_window') or {}).get('fields', {}).get('opacity_270'),
                      'pressed': None, 'repeat': None})
            cur_input = r
        elif e['ev'] == 'exit' and e['name'] in ('pad_pressed', 'pad_repeat') and cur_input is not None \
                and in_input(e['caller_ra']):
            k = 'pressed' if e['name'] == 'pad_pressed' else 'repeat'
            need(cur_input[k] is None, f'two {k} reads in one 0x11e480 call')
            cur_input[k] = e['word']
        elif e['ev'] == 'exit' and e['name'] == 'restore_input':
            cur_input['input_state_out'] = e['restore']['state_1f89a8']
            cur_input = None
        elif e['ev'] == 'exit' and e['name'] == 'restore_update':
            r = rows.setdefault(trav - s_trav, {'t': trav - s_trav})
            r['update_state_out'] = e['restore']['state_1f89a8']
    for d in draw_detail:
        if d['oracle_i'] > successor_i:
            r = rows.setdefault(d['trav'] - s_trav, {'t': d['trav'] - s_trav})
            r['draws'] = r.get('draws', 0) + 1
    out = []
    for t in sorted(rows):
        r = rows[t]
        out.append({'t': t, 'state_in': r.get('input_state_in'), 'state_out': r.get('input_state_out'),
                    'update_state_out': r.get('update_state_out'), 'gate_open': r.get('gate_open'),
                    'pressed': r.get('pressed'), 'draws': r.get('draws', 0), 'vb': r.get('vb'),
                    'opacity_bits': r.get('opacity_bits'), 'repeat': r.get('repeat')})
    return out

def analyse(evidence, out, rng, retire_evidence):
    runs = {r: os.path.join(evidence, 'runs', r) for r in RUNS}
    for r, p in runs.items():
        need(os.path.isdir(p), f'missing run {p}')
    os.makedirs(out)
    det = retire.determinism(runs, RETIRE_CPS + CHECKPOINTS)
    need(all(v['identical'] for k, v in det.items() if isinstance(v, dict) and 'identical' in v),
         'runs are not deterministic: ' + json.dumps({k: v.get('identical') for k, v in det.items() if isinstance(v, dict)}))
    for name in ('memcards-before.json', 'memcards-after.json'):
        for r, p in runs.items():
            m = json.load(open(os.path.join(p, name)))
            need(m['mc0']['entries'] == [] and m['mc1']['entries'] == [], f'{r}: research memory card not empty ({name})')
    a = runs['fill00-a']
    ev = read_jsonl(os.path.join(a, 'events.jsonl'))
    meta = json.load(open(os.path.join(a, 'run-meta.json')))
    need(meta.get('restore_cancel') is True and meta['pad_step'] == 8 and meta['missing_checkpoints'] == [],
         'run-meta does not describe a completed Restore-cancel script')
    need(ev[-1]['ev'] == 'stop' and ev[-1]['kind'] == 'title_constructor_return', 'capture does not end at the second title return')
    need(meta['checkpoints'] == list(RETIRE_CPS) + list(CHECKPOINTS), f'checkpoint order {meta["checkpoints"]}')
    tl, detail, counts = timeline(ev)
    for r in RUNS[1:]:
        need(timeline(read_jsonl(os.path.join(runs[r], 'events.jsonl')))[0] == tl, f'timeline differs in {r}')
    cps = checkpoint_boundaries(ev)
    need(tuple(cps) == RETIRE_CPS + CHECKPOINTS, f'checkpoints {list(cps)}')
    successor_i = cps['retire-successor-entry'][1]
    rng_rows = read_jsonl(rng)
    draws, draw_detail = rng_draws(ev, rng_rows, successor_i)
    ptrs = table_ptrs(ev)
    need(len(ptrs) == 2, 'expected two selector-1 title constructions')

    # traversal ordinals (count of 0x100618 scheduler calls) and the construction traversal S
    trav_of = {}
    trav = 0
    for e in ev:
        if e['ev'] == 'top' and e['pc'] == SCHEDULER_PC:
            trav += 1
        trav_of[e['i']] = trav
    s_trav = trav_of[successor_i]

    write_x(os.path.join(out, 'events-detail.jsonl'), jl(detail))
    cp_info = {}
    for name in RETIRE_CPS + CHECKPOINTS:
        e, boundary = cps[name]
        fs = read_jsonl(os.path.join(a, 'checkpoints', name, 'state.jsonl'))[0]
        pre = prefix(detail, tl, boundary)
        resource = [x for x in pre if x['kind'] != 'phase']
        info = {'oracle_i': e['i'], 'boundary_oracle_i': boundary, 'vb': e['vb'], 'pc': e['pc'],
                'traversal': trav_of[e['i']], 't': trav_of[e['i']] - s_trav, 'resource_events': len(resource),
                'events_by_kind': dict(sorted(collections.Counter(
                    x['kind'] if x['kind'] not in ('effect', 'mode', 'ui', 'glyph') else x['kind'] + ':' + x['op']
                    for x in resource).items())),
                'rng_draws_before': sum(1 for d in draw_detail if d['oracle_i'] < boundary),
                'gs': e['gs'], 'stack_depth': e['stack_depth'], 'rand_state': e['rand_state_214ec8_58']}
        cp_info[name] = info
        if name in RETIRE_CPS:
            continue
        d = os.path.join(out, name)
        os.makedirs(d)
        write_x(os.path.join(d, 'events.jsonl'), jl(pre))
        st = native_state(fs, name, ptrs)
        write_x(os.path.join(d, 'state.json'), json.dumps(st, separators=(',', ':')) + '\n')
        fx = {'effect106': effect_state(fs['title_effect']) if fs['title_effect_slot'] >= 0 else None}
        write_x(os.path.join(d, 'effects.json'), json.dumps(fx, separators=(',', ':')) + '\n')
        write_x(os.path.join(d, 'rng.jsonl'), jl(draws[:info['rng_draws_before']]))
        cpd = {r: os.path.join(runs[r], 'checkpoints', name) for r in RUNS}
        info['vram'] = retire.vram_analysis(os.path.join(cpd['fill00-a'], 'vram.bin'), os.path.join(cpd['fillff-a'], 'vram.bin'),
                                         os.path.join(cpd['fill00-a'], 'vram-last-writer.bin'),
                                         os.path.join(cpd['fill00-a'], 'vram-upload-ever.bin'), d)
        info['state'] = {'allocations': len(st['allocations']), 'handles': len(st['handles']), 'records': len(st['records']),
                         'hash': len(st['hash']), 'image_list': st['image_list'], 'materials': len(st['materials']),
                         'available': st['available'], 'reserved': len(st['reserved']),
                         'hash_tombstones': sum(1 for _, v in st['hash'] if v == -1)}
        info['effect106'] = fx['effect106']
        info['theme_words_2d9a60'] = fs['theme_words_2d9a60']
    # frames must be ordered (same-traversal checkpoints allowed)
    need(all(cp_info[x]['oracle_i'] < cp_info[y]['oracle_i'] for x, y in zip(CHECKPOINTS, CHECKPOINTS[1:])), 'checkpoint order')

    frames = manager_frames(ev, s_trav, draw_detail, successor_i)
    write_x(os.path.join(out, 'manager-frames.jsonl'), jl(frames))
    write_x(os.path.join(out, 'rng-detail.jsonl'), jl(draw_detail[len(rng_rows):]))
    ref = {'schema': 'okage.restore-original-reference.v1', 'route': 'restore-cancel',
           'checkpoint_rule': 'entry checkpoints precede every event of that call (boundary = entry index, exclusive); '
                              'return checkpoints follow every event of that call (boundary = exit index + 1)',
           'construction_traversal_S': s_trav, 'checkpoints': cp_info, 'timeline_counts': dict(sorted(counts.items()))}
    ref.update(route_facts(ev, detail, frames, draw_detail, cps, trav_of, s_trav, successor_i, runs))
    ref['determinism'] = det
    ref['retire_neutrality'] = retire_neutrality(a, ev, tl, detail, cps, out, retire_evidence, runs)
    ctl = os.path.join(evidence, 'runs', CONTROL)
    if os.path.isdir(ctl):
        ref['control_minimal'] = control(ctl, a, ev, cps, successor_i)
    return ref

# ----------------------------------------------------------------------------------------------
def route_facts(ev, detail, frames, draw_detail, cps, trav_of, s_trav, successor_i, runs):
    enters = {e['i']: e for e in ev if e['ev'] == 'enter'}
    exits = {e['enter']: e for e in ev if e['ev'] == 'exit'}
    post = [e for e in ev if e['i'] > successor_i]
    ent = lambda n: [e for e in post if e['ev'] == 'enter' and e['name'] == n]  # noqa: E731
    t = lambda e: trav_of[e['i']] - s_trav  # noqa: E731
    f = {}
    # pad
    script = [{k: e[k] for k in ('i', 'vb', 'step', 'buttons', 'why')} | {'t': t(e) if e['i'] > successor_i else None}
              for e in ev if e['ev'] == 'pad_script']
    pad_rows = []
    for e in post:
        if e['ev'] == 'exit' and e['name'] in ('pad_pressed', 'pad_repeat', 'pad_analog'):
            if in_input(e['caller_ra']):
                pad_rows.append({'oracle_i': e['i'], 'vb': e['vb'], 't': t(e), 'fn': e['name'], 'caller_ra': e['caller_ra'],
                                 'word': e.get('word'), 'outputs': e.get('outputs')})
    write_x(os.path.join(OUT[0], 'pad-consumption.jsonl'), jl(pad_rows))
    consumed = [r for r in pad_rows if r['fn'] == 'pad_pressed' and r['word']]
    need(len(consumed) == 1 and consumed[0]['word'] == 0x20 and consumed[0]['caller_ra'] == '0x11f08c',
         f'expected exactly one consumed Cancel (0x20 at 0x11f084): {consumed}')
    nonneutral = [r for r in pad_rows if (r['word'] or (r['outputs'] and r['outputs'] != [128] * 4))]
    f['pad'] = {'script_actions': script, 'host_pad_changes': read_jsonl(os.path.join(runs['fill00-a'], 'pad-host.jsonl')),
                'restore_pad_reads': len(pad_rows), 'nonneutral_reads': nonneutral, 'cancel_consumed': consumed[0],
                'cancel_mask_1f85e0': '0x20'}
    # state machine
    states = [x for x in detail if x['kind'] == 'ui' and x['op'] == 'state']
    f['restore_states'] = [{'state': x['state'], 'oracle_i': x['oracle_i'], 'vb': x['vb'], 't': x['trav'] - s_trav,
                            'at': x['fn']} for x in states]
    death = [r for r in frames if r['state_in'] is not None and r['state_out'] == 0 and r['state_in'] < 0]
    need(len(death) == 1, 'expected one death traversal')
    cancel = [r for r in frames if r['pressed'] == 0x20]
    need(len(cancel) == 1, 'expected one Cancel traversal')
    f['manager_frames'] = {'rows': len(frames), 'first_t': frames[0]['t'], 'last_t': frames[-1]['t'],
                           'contiguous': [r['t'] for r in frames] == list(range(frames[0]['t'], frames[-1]['t'] + 1)),
                           'empty_ready_t': cps_t(cps, 'empty-ready', trav_of, s_trav), 'cancel_t': cancel[0]['t'],
                           'death_t': death[0]['t'], 'death_minus_cancel': death[0]['t'] - cancel[0]['t'],
                           'gate_closed_frames': [r['t'] for r in frames if r['gate_open'] is False]}
    # RNG
    after = [d for d in draw_detail if d['oracle_i'] > successor_i]
    by_t = collections.Counter(d['trav'] - s_trav for d in after)
    f['rng'] = {'draws_total': len(draw_detail), 'draws_after_retire_successor_entry': len(after),
                'by_site': dict(collections.Counter(d['ra'] for d in after)),
                'by_phase': dict(collections.Counter(d['phase'] for d in after)),
                'by_traversal': {str(k): v for k, v in sorted(by_t.items())},
                'by_checkpoint_interval': {}, 'state_at_stop': ev[-1]['rand_state_214ec8_58'],
                # hooked-frame stack, card-thread frames (storage_probe, sp 0x2f2d..) removed: the observer keeps one stack
                # across EE threads, so a pending card-thread call can appear under a main-thread draw
                'callers': dict(collections.Counter('/'.join([x for x in d['frames'] if x != 'storage_probe'][-2:]) for d in after))}
    prev = 'retire-successor-entry'
    for cp in CHECKPOINTS[1:]:
        lo, hi = cps[prev][1], cps[cp][1]
        f['rng']['by_checkpoint_interval'][f'{prev}..{cp}'] = sum(1 for d in after if lo <= d['oracle_i'] < hi)
        prev = cp
    # atlas
    al = ent('atlas_load')
    need(len(al) == 1, 'one bm_if atlas load')
    alx = exits[al[0]['i']]
    inner = [x for x in detail if al[0]['i'] < x['oracle_i'] < alx['i'] and x['kind'] != 'phase']
    atlas_h = alx['handle']
    rel = [x for x in detail if x['kind'] == 'retire' and x['handle'] == atlas_h and x['oracle_i'] > alx['i']]
    need(len(rel) == 1, 'atlas handle retired exactly once')
    wc = ent('wrapper_cleanup')[0]
    wcx = exits[wc['i']]
    in_wc = [x for x in detail if wc['i'] < x['oracle_i'] < wcx['i'] and x['kind'] != 'phase']
    calls_wc = [{'name': e['name'], 'args0': e['args'][0], 'ra': e['ra']} for e in post
                if e['ev'] == 'enter' and wc['i'] < e['i'] < wcx['i'] and e['depth'] == wc['depth'] + 1]
    f['atlas'] = {'handle': atlas_h, 'record': alx['record'], 'wrapper_30': exits[ent('restore_resources_load')[0]['i']].get('v0'),
                  'load_events': inner, 'release_sequence_in_0x124448': in_wc, 'calls_in_0x124448': calls_wc,
                  'retire_oracle_i': rel[0]['oracle_i'], 'retire_t': rel[0]['trav'] - s_trav}
    # window/manager/wrapper order
    seq = [{k: x[k] for k in x if k in ('kind', 'op', 'node', 'mode', 'selector', 'window', 'state', 'handle', 'alloc', 'slot')}
           | {'oracle_i': x['oracle_i'], 't': x['trav'] - s_trav, 'vb': x['vb']}
           for x in detail if x['oracle_i'] > successor_i and x['kind'] in ('mode', 'ui', 'retire', 'free', 'effect', 'glyph')]
    f['restore_sequence'] = seq
    # second title
    st2 = [e for e in post if e['ev'] == 'enter' and e['name'] == 'mode8_construct']
    need(len(st2) == 1 and st2[0]['selector'] == 1, 'second title construction')
    second = [x for x in detail if x['oracle_i'] > st2[0]['i'] and x['kind'] != 'phase']
    f['second_title'] = {
        'handles': [x['handle'] for x in second if x['kind'] == 'handle'],
        'records': [x['record'] for x in second if x['kind'] == 'handle'],
        'allocations': [x['alloc'] for x in second if x['kind'] == 'alloc'],
        'reserve': [x for x in second if x['kind'] == 'reserve'],
        'materials': [x['handle'] for x in second if x['kind'] == 'material'],
        'events_by_kind': dict(collections.Counter(x['kind'] for x in second)),
        'alloc_placement': [{'alloc': x['alloc'], 'tbp': x['tbp'], 'psm': x['psm'], 'w': x['w'], 'h': x['h'],
                             'caller_ra': x['caller_ra']} for x in second if x['kind'] == 'alloc'],
    }
    gs = [json.loads(line) for line in open(os.path.join(runs['fill00-a'], 'gs-ledger.jsonl'))]
    tr = [g for g in gs if g['kind'] == 'gs_transfer']
    n0 = cps_gs(cps, 'retire-successor-entry')
    post_tr = [g for g in tr if g['seq'] > n0]
    write_x(os.path.join(OUT[0], 'gs-writes.jsonl'), jl(post_tr))
    f['gs_writes'] = {'transfers_at_checkpoint': {cp: cps[cp][0]['gs']['transfers'] for cp in CHECKPOINTS},
                      'after_retire_successor_entry': len(post_tr),
                      'restore_window_transfers': [{k: g[k] for k in ('seq', 'dbp', 'dpsm', 'dsax', 'dsay', 'rrw', 'rrh',
                                                                       'expected_bytes', 'image_sha256')}
                                                   for g in post_tr if g['seq'] <= cps_gs(cps, 'wrapper-free')],
                      'local_to_local': sum(1 for g in gs if g['kind'] == 'gs_local_to_local')}
    uploads_post = [x for x in detail if x['oracle_i'] > successor_i and x['kind'] == 'upload-bytes']
    f['gs_writes']['ee_upload_rows_after_retire_successor_entry'] = len(uploads_post)
    # glyph verdict
    glyph_hits = {n: len(ent(n)) for n in ('glyph_cache_transfer', 'glyph_cache_lookup', 'glyph_path_195ff0', 'glyph_cache')}
    gstates = [e for e in ev if e['ev'] == 'glyph_state']
    f['glyph'] = {'hook_hits_after_retire_successor_entry': glyph_hits,
                  'font_flag_words': sorted(set(tuple(g['font_flag_words']) for g in gstates)),
                  'cache_plus8_nonneg': sorted(set(g['cache_plus8_nonneg'] for g in gstates)),
                  'transfers_not_from_allocator_uploads': len(post_tr) - len(uploads_post),
                  'verdict': None}
    f['glyph']['verdict'] = ('no glyph-cache or other non-allocator GS write between the Retire successor entry and the stop'
                             if not any(glyph_hits.values()) and f['glyph']['cache_plus8_nonneg'] == [0]
                             and all(not (int(w, 16) & 0x100) for ws in f['glyph']['font_flag_words'] for w in ws if w)
                             and f['glyph']['transfers_not_from_allocator_uploads'] == 0
                             else 'GLYPH/OTHER WRITES PRESENT: see hook hits and gs-writes.jsonl')
    # tails
    rec = [x for x in detail if x['kind'] == 'effect' and x['op'] == 'reclaim']
    th = [e for e in post if e['ev'] == 'enter' and e['name'] in ('theme_end_12de98', 'theme_release_12dcb0', 'theme_start')]
    tt = [e for e in post if e['ev'] == 'enter' and e['name'] == 'theme_tick']
    f['tails'] = {'effect106_reclaim': [{'slot': x['slot'], 'oracle_i': x['oracle_i'], 't': x['trav'] - s_trav, 'vb': x['vb'],
                                         'caller_ra': x['caller_ra']} for x in rec],
                  'theme_events': [{'name': e['name'], 'ra': e['ra'], 't': t(e), 'vb': e['vb'],
                                    'theme_words_2d9a60': e.get('theme_words_2d9a60')} for e in th],
                  'theme_words_first_tick': tt[0]['theme_words_2d9a60'] if tt else None,
                  'theme_words_last_tick': tt[-1]['theme_words_2d9a60'] if tt else None,
                  'theme_ticks': len(tt)}
    f['open_questions'] = open_questions(ev, post, enters, exits, detail, cps, trav_of, s_trav, successor_i, frames, f)
    return f

OUT = [None]

def cps_t(cps, cp, trav_of, s_trav):
    return trav_of[cps[cp][0]['i']] - s_trav

def cps_gs(cps, cp):
    return cps[cp][0]['gs']['transfers']

def open_questions(ev, post, enters, exits, detail, cps, trav_of, s_trav, successor_i, frames, f):
    """Answers to O01..O15 of the Restore input contract from this capture (settled/unsettled with evidence)."""
    t = lambda e: trav_of[e['i']] - s_trav  # noqa: E731
    ent = lambda n: [e for e in post if e['ev'] == 'enter' and e['name'] == n]  # noqa: E731
    probes = lambda n: [e for e in post if e['ev'] == 'probe' and e['name'] == n]  # noqa: E731
    q = {}
    s_rows = [e for e in post if t(e) == 0 and e['ev'] == 'enter' and e['name'] in (
        'wrapper_tick', 'resource_tick', 'window_opacity_step', 'opacity_get', 'window_mode_set', 'mc_check_16d580',
        'restore_input', 'restore_update', 'ui_layout', 'ui_layout_mode0', 'ui_layout_mode2')]
    s_draws = sum(1 for r in frames if r['t'] == 0 for _ in range(r['draws']))
    q['O01'] = {'settled': True, 'traversal_S': s_trav,
                'order_in_S': [{'name': e['name'], 'ra': e['ra'], 'oracle_i': e['i']} for e in s_rows],
                'note': 'entry events of the requested call sites in traversal S (0x198168 = entry of its callee 0x1998a8; '
                        '0x11e670 = 0x1a4e50 (f0 at its return); 0x11effc = 0x1a99e0; 0x11c9c4 = 0x16d580)',
                'opacity_get_f0_in_S': [exits[e['i']]['f0_bits'] for e in s_rows if e['name'] == 'opacity_get']}
    alx = exits[ent('atlas_load')[0]['i']]
    inner_allocs = [e for e in post if e['ev'] == 'exit' and e['name'] == 'alloc' and ent('atlas_load')[0]['i'] < e['i'] < alx['i']]
    q['O02'] = {'settled': True, 'atlas_handle_0x11fb9c_and_0x124400': alx['handle'], 'record': alx['record'],
                'allocations': [{'id': e['result']['id'], 'tbp': e['result']['tbp'], 'psm': e['result']['psm'],
                                 'w': e['result']['w'], 'h': e['result']['h'], 'caller_ra': enters[e['enter']]['ra']}
                                for e in inner_allocs],
                'handle_create_record': [exits[e['i']]['record'] for e in post if e['ev'] == 'enter' and e['name'] == 'handle_create'
                                         and ent('atlas_load')[0]['i'] < e['i'] < alx['i']]}
    sv = [e for e in post if e['ev'] == 'enter' and e['name'] == 'effect_service']
    first_s = sv[0] if sv else None
    in_s = [e for e in post if first_s and first_s['i'] < e['i'] < exits[first_s['i']]['i']]
    q['O03'] = {'settled': bool(first_s),
                'frees': [{'a0': e['args'][0], 'ra': e['ra'], 'job_slot': e.get('job_slot')} for e in in_s
                          if e['ev'] == 'enter' and e['name'] == 'effect_free'],
                'slot_after': exits[first_s['i']]['effect'] if first_s else None,
                'texture_calls_between': [e['name'] for e in in_s if e['ev'] == 'enter' and e['name'] in ('handle_desc_get', 'handle_retire')],
                't': t(first_s) if first_s else None}
    lay = ent('ui_layout')
    q['O04'] = {'settled': True, 'draws_per_traversal': f['rng']['by_traversal'], 'by_site': f['rng']['by_site'],
                'draws_in_S': s_draws,
                'rate_check_probe_0x1984b0': [{'t': t(e), 'window': e['window']} for e in probes('ui_layout_rate_check')][:3],
                'rate_check_probe_hits': len(probes('ui_layout_rate_check')), 'ui_layout_calls': len(lay),
                'layout_calls_per_traversal_mode0': dict(collections.Counter(str(t(e)) for e in ent('ui_layout_mode0'))),
                'layout_calls_per_traversal_mode2': dict(collections.Counter(str(t(e)) for e in ent('ui_layout_mode2'))),
                'a2_at_0x1a47e8_values': sorted(set(e['args'][2] for e in ent('ui_place_19ca20') if e['ra'] == '0x1a47f0')),
                'a6_at_0x1a385c_values': sorted(set(e['args'][6] for e in ent('ui_place_19ca20') if e['ra'] == '0x1a3864'))}
    mc = [e for e in post if e['ev'] in ('enter', 'exit') and e['name'] in ('mc_job_wait_16d4f8', 'mc_check_16d580', 'mc_16d748', 'mc_sync_16c2a0')]
    syncs = [e for e in post if e['ev'] == 'exit' and e['name'] == 'mc_sync_16c2a0']
    q['O05'] = {'settled': True,
                'job_wait_16d4f8_a0': sorted(set(e['args'][0] for e in ent('mc_job_wait_16d4f8'))),
                'sync_returns_by_caller': dict(collections.Counter(f"{enters[e['enter']]['ra']}:{int(e['v0'], 16)}" for e in syncs)),
                'state_0x212_t': next((s['t'] for s in f['restore_states'] if s['state'] == 0x212), None),
                'state_0x21c_t': next((s['t'] for s in f['restore_states'] if s['state'] == 0x21c), None),
                'w24eea8_w24ee78_w24ee7c_at_empty_ready': [
                    (e['restore']['w24eea8'], e['restore']['w24ee78'], e['restore']['w24ee7c'])
                    for e in post if e['ev'] == 'enter' and e['name'] == 'restore_input' and e['i'] == cps['empty-ready'][1]],
                'storage_calls': len(mc)}
    rs = ent('mode_result_set')
    q['O06'] = {'settled': True, 'cancel': f['pad']['cancel_consumed'], 'cancel_t': f['manager_frames']['cancel_t'],
                'death_t': f['manager_frames']['death_t'], 'death_minus_cancel': f['manager_frames']['death_minus_cancel'],
                'mode_result_set_args': [{'a0': e['args'][0], 'a1': e['args'][1], 'a2': e['args'][2], 'ra': e['ra'], 't': t(e)} for e in rs],
                'opacity_per_frame': [[r['t'], r['opacity_bits']] for r in frames if r['t'] >= f['manager_frames']['cancel_t']]}
    upd = [e for e in post if e['ev'] == 'exit' and e['name'] == 'manager_update_pass']
    q['O07'] = {'settled': bool(upd), 'state_at_last_0x120ad0_return': upd[-1]['state_1f89a8'] if upd else None,
                'flags_at_last_0x120ad0_return': upd[-1]['flags'] if upd else None, 't': t(upd[-1]) if upd else None}
    wt = [(e, exits.get(e['i'])) for e in ent('wrapper_tick')]
    phase1 = next(((t(e), x['wrapper']['w2c_phase']) for e, x in wt if x and x['wrapper']['w2c_phase'] == 1), None)
    req = [e for e in ent('mode_request')]
    # 0x124868: the root tick that unlinks the dead wrapper (+0x38 child wrapper at entry, 0 at return, v0 0x80000000)
    wrapper = ent('successor_construct')[0]['node'] if ent('successor_construct') else enters[successor_i]['node']
    pt = [x for x in post if x['ev'] == 'exit' and x['name'] == 'mode_parent_tick' and x['child'] == '0x0'
          and enters[x['enter']]['child'] == wrapper]
    q['O08'] = {'settled': bool(phase1 and req and pt),
                'wrapper_phase1_first_seen_t': phase1[0] if phase1 else None,
                'mode_requests': [{'mode': e['mode'], 'selector': e['selector'], 'arg3': e['arg3'], 'ra': e['ra'], 't': t(e)} for e in req],
                'wrapper_marked_dead_t': t(pt[0]) if pt else None}
    wc = ent('wrapper_cleanup')[0]
    wcx = exits[wc['i']]
    q['O09'] = {'settled': True,
                'handle_retire_a0': [e['handle_or_record'] for e in post if e['ev'] == 'enter' and e['name'] == 'handle_retire' and wc['i'] < e['i'] < wcx['i']],
                'free_a0': [e['desc']['raw'] for e in post if e['ev'] == 'enter' and e['name'] == 'free' and wc['i'] < e['i'] < wcx['i']],
                'probe_199ed4_taken_hits': len(probes('ui_release_199ed4_taken')),
                'probe_199ed4_not_taken_hits': len(probes('ui_release_199ed4_not_taken')),
                'taken_inside_0x124448': [e['i'] for e in probes('ui_release_199ed4_taken') if wc['i'] < e['i'] < wcx['i']],
                't': t(wc)}
    q['O10'] = {'settled': True, 'hook_hits': f['glyph']['hook_hits_after_retire_successor_entry'],
                'font_flag_words': f['glyph']['font_flag_words'], 'cache_plus8_nonneg': f['glyph']['cache_plus8_nonneg'],
                'verdict': f['glyph']['verdict']}
    q['O11'] = {'settled': True, 'second_title_handles': f['second_title']['handles'], 'records': f['second_title']['records'],
                'reserve': f['second_title']['reserve'], 'materials': f['second_title']['materials'],
                'note': 'hash slot of every record is in second-title-return/state.json (records[].hash_slot, hash)'}
    q['O12'] = {'settled': True, 'allocations': f['second_title']['alloc_placement'],
                'allocator_dump': 'second-title-return/state.json (descriptors, free queue, cursors, occupancy)'}
    q['O13'] = {'settled': True, 'theme_words_at_S': f['tails']['theme_words_first_tick'],
                'theme_end_events': f['tails']['theme_events']}
    l0 = [e for e in post if e['ev'] == 'enter' and e['name'] == 'ui_layout_19d0c8']
    q['O14'] = {'settled': True, 'draws_in_S': s_draws, 'calls_0x19d0c8': len(l0),
                'w2698_w3bf0_at_0x19d0c8': sorted(set(((e['window'] or {}).get('w2698'), (e['window'] or {}).get('w3bf0')) for e in l0)),
                'mode0_layouts_in_S': [t(e) for e in ent('ui_layout_mode0')][:4],
                'note': '0x19d0c8 entry hook (window fields read at the entry of that function)'}
    pk = {}
    for n in ('packet_hook_return_1aedfc', 'packet_hook_return_1af284', 'packet_hook_return_1af6ec', 'packet_hook_return_1afa10'):
        hits = probes(n)
        pk[n] = {'hits': len(hits), 'hook_202ddc_values': sorted(set(e['hook_202ddc'] for e in hits))}
    sched = collections.Counter(f"{e['name']}@{e['ra']}" for e in post if e['ev'] == 'enter' and e['ra'] in ('0x115008', '0x11528c', '0x114ff4', '0x115384'))
    q['O15'] = {'settled': 'partial', 'packet_hook_returns': pk, 'scheduler_callback_entries': dict(sched),
                'fn_175398_calls': len(ent('fn_175398')),
                'note': 'targets are read from *0x202ddc at the jalr+8 block start (the register itself is not '
                        'observable there); a return site never reported is "not observed", not "not reached"'}
    return q

# ----------------------------------------------------------------------------------------------
def retire_neutrality(a, ev, tl, detail, cps, out, retire_evidence, runs):
    """The Restore run must equal the pinned Retire restore capture up to the 0x124548 entry."""
    res = {}
    retire_run = os.path.join(retire_evidence, 'runs', 'restore', 'fill00-a')
    retire_an = os.path.join(retire_evidence, 'analysis', 'restore', 'successor-entry')
    boundary = cps['retire-successor-entry'][1]
    mine = [line for line in open(os.path.join(a, 'events.jsonl')) if json.loads(line)['i'] <= boundary + 1]
    theirs = [line for line in open(os.path.join(retire_run, 'events.jsonl'))][:len(mine)]
    rename = lambda s: s.replace('"name":"retire-', '"name":"')  # noqa: E731
    res['raw_events_through_successor_checkpoint_equal_modulo_retire_prefix'] = [rename(x) for x in mine] == theirs
    res['raw_events_compared'] = len(mine)
    pre = prefix(detail, tl, boundary)
    retire_rows = read_jsonl(os.path.join(retire_an, 'events.jsonl'))
    res['lifecycle_equal_retire_successor_entry'] = retire_view(pre) == [
        dict(r, name=r['name'].replace('checkpoint-', 'checkpoint-retire-')) if r['kind'] == 'phase' and r['name'].startswith('checkpoint-') else r
        for r in retire_rows]
    d = os.path.join(out, 'retire-successor-entry')
    s_mine = json.load(open(os.path.join(d, 'state.json')))
    s_retire = json.load(open(os.path.join(retire_an, 'state.json')))
    s_mine.pop('coverage')
    s_retire.pop('coverage')
    res['state_equal'] = s_mine == s_retire
    for n in ('effects.json', 'rng.jsonl', 'vram-written-mask.bin', 'vram-written-values.bin', 'vram-invalidated-mask.bin'):
        res[n + '_equal'] = fsha(os.path.join(d, n)) == fsha(os.path.join(retire_an, n))
    for r in RUNS:
        cd = os.path.join(runs[r], 'checkpoints', 'retire-successor-entry')
        td = os.path.join(retire_evidence, 'runs', 'restore', r, 'checkpoints', 'successor-entry')
        res[f'{r}: state.jsonl (fill fields removed)'] = retire.strip_fill(read_jsonl(os.path.join(cd, 'state.jsonl'))[0]) == \
            retire.strip_fill(read_jsonl(os.path.join(td, 'state.jsonl'))[0])
        for n in ('vram.bin', 'vram-last-writer.bin', 'vram-upload-ever.bin'):
            res[f'{r}: {n}'] = fsha(os.path.join(cd, n)) == fsha(os.path.join(td, n))
    res['failures'] = [k for k, v in res.items() if v is False]
    return res

def control(ctl, a, ev, cps, successor_i):
    cev = read_jsonl(os.path.join(ctl, 'events.jsonl'))
    cmeta = json.load(open(os.path.join(ctl, 'run-meta.json')))
    calls = lambda evs, upto: sum(1 for e in evs if e['ev'] == 'enter' and e.get('name') == 'restore_input' and e['i'] <= upto)  # noqa: E731
    cck = {e['name']: e for e in cev if e['ev'] == 'checkpoint'}
    hck = {e['name']: e for e in ev if e['ev'] == 'checkpoint'}
    out = {'vblanks': cmeta['vblanks'], 'checkpoints': cmeta['checkpoints'], 'missing': cmeta['missing_checkpoints']}
    cmp = {}
    for name in ('retire-successor-entry', 'empty-ready', 'second-title-return'):
        cs = read_jsonl(os.path.join(ctl, 'checkpoints', name, 'state.jsonl'))[0]
        hs = read_jsonl(os.path.join(a, 'checkpoints', name, 'state.jsonl'))[0]
        cmp[name] = {'resource_state_equal': retire.resource_equal(cs, hs, tuple(k for k in retire.RESOURCE_STATE_KEYS
                                                                                 if k not in ('allocations', 'allocation_serials'))),
                     'restore_input_calls_before_equal': calls(cev, cck[name]['i']) == calls(ev, hck[name]['i']),
                     'restore_input_calls_before': [calls(cev, cck[name]['i']), calls(ev, hck[name]['i'])],
                     'vram_fill00_equal': fsha(os.path.join(ctl, 'checkpoints', name, 'vram.bin'))
                     == fsha(os.path.join(a, 'checkpoints', name, 'vram.bin')),
                     'vb_control': cck[name]['vb'], 'vb_hooked': hck[name]['vb']}
    out['checkpoints_compared'] = cmp
    cr = [(e['restore']['state_1f89a8'], e['input_gate_open']) for e in cev if e['ev'] == 'enter' and e.get('name') == 'restore_input']
    hr = [(e['restore']['state_1f89a8'], e['input_gate_open']) for e in ev if e['ev'] == 'enter' and e.get('name') == 'restore_input']
    out['restore_input_sequence_equal'] = cr == hr
    out['restore_input_calls'] = [len(cr), len(hr)]
    out['first_difference'] = next((i for i, (x, y) in enumerate(zip(cr, hr)) if x != y), None)

    def transitions(seq):
        o, last = [], None
        for k, s in enumerate(seq):
            if s != last:
                o.append([k, s[0], s[1]])
                last = s
        return o
    out['state_gate_transitions_by_call_control'] = transitions(cr)
    out['state_gate_transitions_by_call_hooked'] = transitions(hr)
    out['note'] = ('Restore traversal counts to card readiness are observer-dependent (asynchronous card thread vs EE '
                   'breakpoint density); the Cancel-to-death interval, the call count to the stop and every resource '
                   'state, VRAM and the pad stream are compared above')
    out['final_vram_equal'] = fsha(os.path.join(ctl, 'vram.bin')) == fsha(os.path.join(a, 'vram.bin'))
    out['pad_host_equal_modulo_vb'] = [{k: r[k] for k in ('buttons', 'play_mask')} for r in read_jsonl(os.path.join(ctl, 'pad-host.jsonl'))] \
        == [{k: r[k] for k in ('buttons', 'play_mask')} for r in read_jsonl(os.path.join(a, 'pad-host.jsonl'))]
    return out

def regression(retire_run, title_run, default_run, retire_evidence, title_runs, startup_run):
    out = {}
    if retire_run:
        ref = os.path.join(retire_evidence, 'runs', 'restore', 'fill00-a')
        r = {}
        for n in ('events.jsonl', 'disc-opens.jsonl', 'pad-host.jsonl', 'vram.bin', 'vram-last-writer.bin', 'vram-upload-ever.bin',
                  'ee-ram-32mb.bin', 'ee-ram-after-10bc78.bin', 'final-state.jsonl', 'state-at-100618.jsonl'):
            r[n] = fsha(os.path.join(retire_run, n)) == fsha(os.path.join(ref, n))
        r['gs-ledger.jsonl (ee_event removed)'] = title.ledger_without_ee_cross_reference(os.path.join(retire_run, 'gs-ledger.jsonl')) \
            == title.ledger_without_ee_cross_reference(os.path.join(ref, 'gs-ledger.jsonl'))
        for cp in ('title-constructed', 'first-update', 'stable-menu', 'removal-request', 'resource-retirement', 'successor-entry'):
            for n in ('state.jsonl', 'vram.bin', 'vram-last-writer.bin', 'vram-upload-ever.bin'):
                r[f'checkpoints/{cp}/{n}'] = fsha(os.path.join(retire_run, 'checkpoints', cp, n)) == fsha(os.path.join(ref, 'checkpoints', cp, n))
        ma, mb = (json.load(open(os.path.join(p, 'run-meta.json'))) for p in (retire_run, ref))
        ma.pop('wall_seconds')
        mb.pop('wall_seconds')
        r['run-meta.json (wall_seconds removed)'] = ma == mb
        out['retire_restore_mode_vs_retire_fill00_a'] = r
    if title_run or default_run:
        out.update(retire.regression(title_run, default_run, title_runs, startup_run))
    return out

LCG_MUL, LCG_ADD = 0x41c64e6d, 0x3039
LIGHT_PHASE = 'restore-ui'

def lcg(s):
    s = (s * LCG_MUL + LCG_ADD) & 0xffffffff
    return s, s & 0x7fffffff

def lcg_steps(a, b, limit=1 << 20):
    n, s = 0, a
    while s != b:
        s, _ = lcg(s)
        n += 1
        need(n <= limit, f'rand state {b} not reachable from {a} within {limit} LCG steps')
    return n

def verify_lcg(rng_rows):
    """The derivation rule must reproduce the observed RNG draws (state chain and value = state & 0x7fffffff)."""
    for x, y in zip(rng_rows, rng_rows[1:]):
        need(lcg(x['state']) == (y['state'], y['value']), f'LCG rule does not reproduce RNG draw {y["i"]}')
    return len(rng_rows) - 1

def light_t(ev, successor_i, end_i):
    """Traversal ordinal in light mode: inside the window, t = 0 until the first 0x11e480 entry, then
    (0x11e480 entries so far) - 1 (one input call per traversal from S to the death traversal D); after the window,
    t_D + scheduler calls (0x100618) since the window end. Before the successor entry: None."""
    out = {}
    calls = 0
    t_end = None
    tops = 0
    for e in ev:
        if e['i'] < successor_i:
            out[e['i']] = None
            continue
        if e['i'] <= end_i:
            if e['ev'] == 'enter' and e.get('name') == 'restore_input':
                calls += 1
            out[e['i']] = max(0, calls - 1)
            t_end = out[e['i']]
        else:
            if e['ev'] == 'top' and e['pc'] == SCHEDULER_PC:
                tops += 1
            out[e['i']] = t_end + tops
    return out

def analyse_light(evidence, out, rng, retire_evidence, full_analysis, control_run):
    runs = {r: os.path.join(evidence, 'runs', r) for r in RUNS}
    for r, p in runs.items():
        need(os.path.isdir(p), f'missing run {p}')
    os.makedirs(out)
    det = retire.determinism(runs, RETIRE_CPS + CHECKPOINTS)
    need(all(v['identical'] for k, v in det.items() if isinstance(v, dict) and 'identical' in v),
         'light runs are not deterministic: ' + json.dumps({k: v.get('identical') for k, v in det.items() if isinstance(v, dict)}))
    for name in ('memcards-before.json', 'memcards-after.json'):
        for r, p in runs.items():
            m = json.load(open(os.path.join(p, name)))
            need(m['mc0']['entries'] == [] and m['mc1']['entries'] == [], f'{r}: research memory card not empty ({name})')
    a = runs['fill00-a']
    ev = read_jsonl(os.path.join(a, 'events.jsonl'))
    meta = json.load(open(os.path.join(a, 'run-meta.json')))
    need(meta.get('light_restore') is True and meta['pad_step'] == 8 and meta['missing_checkpoints'] == []
         and meta['light_window_open_at_stop'] is False, 'run-meta does not describe a completed light Restore-cancel run')
    tl, detail, counts = timeline(ev, light=True)
    for r in RUNS[1:]:
        need(timeline(read_jsonl(os.path.join(runs[r], 'events.jsonl')), light=True)[0] == tl, f'timeline differs in {r}')
    cps = checkpoint_boundaries(ev)
    need(tuple(cps) == RETIRE_CPS + CHECKPOINTS, f'checkpoints {list(cps)}')
    successor_i = cps['retire-successor-entry'][1]
    lw = [e for e in ev if e['ev'] == 'light_window']
    need([e['op'] for e in lw] == ['begin', 'end'], 'light window begin/end')
    begin_i, end_i = lw[0]['i'], lw[1]['i']
    need(cps['retire-successor-entry'][0]['i'] < begin_i and cps['cancel-death'][0]['i'] < end_i, 'light window bounds')
    in_window = lambda i: successor_i <= i <= end_i  # noqa: E731
    leaked = [e for e in ev if in_window(e['i']) and (e['ev'] in ('rand', 'rand_exit', 'probe', 'top')
                                                      or (e['ev'] == 'enter' and e['name'] in ('restore_update', 'ui_place_19c568',
                                                          'ui_place_19ca20', 'mc_check_16d580', 'mc_sync_16c2a0',
                                                          'storage_probe', 'pad_repeat', 'effect_service', 'theme_tick')))]
    need(not leaked, f'non-light observations inside the light window: {leaked[:3]}')
    rng_rows = read_jsonl(rng)
    lcg_checked = verify_lcg(rng_rows)
    obs_draws, obs_detail = rng_draws(ev, rng_rows, successor_i)
    need(all(d['oracle_i'] < successor_i or d['oracle_i'] > end_i for d in obs_detail), 'observed draw inside the window')
    # rand-state anchors inside the window, in order
    anchors = [(successor_i, cps['retire-successor-entry'][0]['rand_state_via_ptr'])]
    for e in ev:
        if begin_i <= e['i'] <= end_i and 'rand_state_via_ptr' in e:
            anchors.append((e['i'], e['rand_state_via_ptr']))
    pre_states = [d['state'] for d, x in zip(obs_draws, obs_detail) if x['oracle_i'] < successor_i]
    need(pre_states and anchors[0][1] == pre_states[-1], 'successor rand state is not the last observed draw')
    tt = light_t(ev, successor_i, end_i)
    derived, dd = [], []
    s = anchors[0][1]
    for (i0, s0), (i1, s1) in zip(anchors, anchors[1:]):
        n = lcg_steps(s0, s1)
        for _ in range(n):
            s, v = lcg(s)
            derived.append({'phase': LIGHT_PHASE, 'state': s, 'value': v, 'derived': 'lcg-step'})
            dd.append({'oracle_i': i1 - 0.5, 'trav': tt[i1], 'after_anchor': i0, 'before_anchor': i1})
        need(s == s1, 'derivation mismatch')
    pre = [(d, x) for d, x in zip(obs_draws, obs_detail) if x['oracle_i'] < successor_i]
    post = [(d, x) for d, x in zip(obs_draws, obs_detail) if x['oracle_i'] > end_i]
    draws, draw_detail = [], []
    for d, x in pre:
        draws.append(dict(d))
        draw_detail.append(x)
    for d, x in zip(derived, dd):
        draws.append(dict(d, i=len(draws)))
        draw_detail.append(dict(x, i=len(draw_detail), phase=LIGHT_PHASE, derived='lcg-step'))
    for d, x in post:
        draws.append(dict(d, i=len(draws)))
        draw_detail.append(dict(x, i=len(draw_detail)))
    ptrs = table_ptrs(ev)
    write_x(os.path.join(out, 'events-detail.jsonl'), jl([dict(x, t=tt.get(x['oracle_i'])) for x in detail]))
    cp_info = {}
    for name in RETIRE_CPS + CHECKPOINTS:
        e, boundary = cps[name]
        fs = read_jsonl(os.path.join(a, 'checkpoints', name, 'state.jsonl'))[0]
        prefix_rows = prefix(detail, tl, boundary)
        resource = [x for x in prefix_rows if x['kind'] != 'phase']
        info = {'oracle_i': e['i'], 'boundary_oracle_i': boundary, 'vb': e['vb'], 'pc': e['pc'], 't': tt.get(e['i']),
                'resource_events': len(resource), 'rng_draws_before': sum(1 for d in draw_detail if d['oracle_i'] < boundary),
                'events_by_kind': dict(sorted(collections.Counter(
                    x['kind'] if x['kind'] not in ('effect', 'mode', 'ui', 'glyph') else x['kind'] + ':' + x['op']
                    for x in resource).items())),
                'gs': e['gs'], 'rand_state': e['rand_state_via_ptr']}
        cp_info[name] = info
        if name in RETIRE_CPS:
            continue
        d = os.path.join(out, name)
        os.makedirs(d)
        write_x(os.path.join(d, 'events.jsonl'), jl(prefix_rows))
        st = native_state(fs, name, ptrs)
        write_x(os.path.join(d, 'state.json'), json.dumps(st, separators=(',', ':')) + '\n')
        fx = {'effect106': effect_state(fs['title_effect']) if fs['title_effect_slot'] >= 0 else None}
        write_x(os.path.join(d, 'effects.json'), json.dumps(fx, separators=(',', ':')) + '\n')
        write_x(os.path.join(d, 'rng.jsonl'), jl(draws[:info['rng_draws_before']]))
        need(not draws[:info['rng_draws_before']] or draws[info['rng_draws_before'] - 1]['state'] == e['rand_state_via_ptr'],
             f'{name}: last draw before the checkpoint is not the checkpoint rand state')
        cpd = {r: os.path.join(runs[r], 'checkpoints', name) for r in RUNS}
        info['vram'] = retire.vram_analysis(os.path.join(cpd['fill00-a'], 'vram.bin'), os.path.join(cpd['fillff-a'], 'vram.bin'),
                                         os.path.join(cpd['fill00-a'], 'vram-last-writer.bin'),
                                         os.path.join(cpd['fill00-a'], 'vram-upload-ever.bin'), d)
        info['state'] = {'allocations': len(st['allocations']), 'handles': len(st['handles']), 'records': len(st['records']),
                         'hash': len(st['hash']), 'available': st['available']}
        info['effect106'] = fx['effect106']
    # manager frames
    rows = {}
    cur = None
    for e in ev:
        if not (successor_i <= e['i'] <= end_i):
            continue
        if e['ev'] == 'enter' and e['name'] == 'restore_input':
            t = tt[e['i']]
            need(t not in rows, f'two 0x11e480 calls in traversal {t}')
            cur = rows[t] = {'t': t, 'state_in': e['restore']['state_1f89a8'], 'gate_open': e['input_gate_open'],
                             'pressed': None, 'vb': e['vb'], 'rand_in': e['rand_state_via_ptr'],
                             'opacity_bits': (e['restore'].get('ui_window') or {}).get('fields', {}).get('opacity_270')}
        elif e['ev'] == 'exit' and e['name'] == 'pad_pressed' and cur is not None and in_input(e['caller_ra']):
            need(cur['pressed'] is None, 'two pressed reads in one 0x11e480 call')
            cur['pressed'] = e['word']
        elif e['ev'] == 'exit' and e['name'] == 'restore_input':
            cur['state_out'] = e['restore']['state_1f89a8']
            cur['rand_out'] = e['rand_state_via_ptr']
            need(cur['rand_out'] == cur['rand_in'], f'draw inside 0x11e480 at t {cur["t"]}')
            cur = None
    per_t = collections.Counter(x['trav'] for x in dd)
    frames = []
    ts = sorted(rows)
    need(ts == list(range(len(ts))), 'light manager frames not contiguous')
    for k, t in enumerate(ts):
        r = rows[t]
        upd = rows[t + 1]['state_in'] if t + 1 in rows else lw[1]['state_1f89a8']
        frames.append({'t': t, 'state_in': r['state_in'], 'state_out': r['state_out'], 'update_state_out': upd,
                       'gate_open': r['gate_open'], 'pressed': r['pressed'], 'draws': per_t.get(t, 0), 'vb': r['vb'],
                       'opacity_bits': r['opacity_bits'], 'rand_in': r['rand_in'], 'update_state_out_source':
                       'next 0x11e480 entry' if t + 1 in rows else 'light_window end (cancel-death)'})
    need(sum(per_t.values()) == len(derived) and set(per_t) <= set(ts), 'derived draws outside the manager traversals')
    write_x(os.path.join(out, 'manager-frames.jsonl'), jl(frames))
    write_x(os.path.join(out, 'rng-detail.jsonl'), jl(draw_detail[len(rng_rows):]))
    ready = [r['t'] for r in frames if r['update_state_out'] == 0x212][:1] + [r['t'] for r in frames if r['update_state_out'] == 0x21c][:1]
    ref = {'schema': 'okage.restore-light-reference.v1', 'route': 'restore-cancel', 'mode': 'light',
           'light_window': {'from': 'retire-successor-entry', 'to': 'cancel-death', 'begin_oracle_i': begin_i, 'end_oracle_i': end_i,
                            'breakpoint_stops_in_window': lw[1]['stops_in_window'], 'armed_at_begin': lw[0]['armed'],
                            'allowed_at_begin': lw[0]['allowed']},
           'derivation': {'rule': 'state(n+1) = state(n)*0x41c64e6d + 0x3039 mod 2^32; value = state & 0x7fffffff',
                          'verified_on_rng_draws': lcg_checked, 'anchors': len(anchors), 'derived_draws': len(derived),
                          'labels': 'not observed in the light window (phase "restore-ui", "derived": "lcg-step")'},
           'card_readiness': {'update_t_state_0x212': ready[0] if ready else None, 'update_t_state_0x21c': ready[1] if len(ready) > 1 else None},
           'checkpoints': cp_info, 'timeline_counts': dict(sorted(counts.items())),
           'draws_per_traversal': {str(k): v for k, v in sorted(per_t.items())},
           'pad': {'host_pad_changes': read_jsonl(os.path.join(a, 'pad-host.jsonl')),
                   'script_actions': [{k: e[k] for k in ('i', 'vb', 'step', 'buttons', 'why')} for e in ev if e['ev'] == 'pad_script']},
           'determinism': det, 'rng_total': len(draws)}
    ref['manager_frames'] = {'rows': len(frames), 'anchors': {'empty_ready_t': next((r['t'] for r in frames if r['state_in'] == 0x21c and r['gate_open']), None),
                                                              'cancel_t': next((r['t'] for r in frames if r['pressed'] == 0x20), None),
                                                              'death_t': frames[-1]['t']}}
    ref['retire_neutrality'] = retire_neutrality(a, ev, tl, detail, cps, out, retire_evidence, runs)
    # cross-checks against the full (hooked) reference and the minimal control
    x = {}
    if full_analysis:
        for cp in CHECKPOINTS:
            fa, la = os.path.join(full_analysis, cp), os.path.join(out, cp)
            fr = [r for r in read_jsonl(os.path.join(fa, 'events.jsonl')) if r['kind'] != 'phase']
            lr = [r for r in read_jsonl(os.path.join(la, 'events.jsonl')) if r['kind'] != 'phase']
            x[cp] = {'lifecycle_equal_full': fr == lr,
                     'state_equal_full': json.load(open(os.path.join(fa, 'state.json'))) == json.load(open(os.path.join(la, 'state.json'))),
                     'effects_equal_full': fsha(os.path.join(fa, 'effects.json')) == fsha(os.path.join(la, 'effects.json')),
                     'vram_masks_equal_full': all(fsha(os.path.join(fa, n)) == fsha(os.path.join(la, n)) for n in
                                                  ('vram-written-mask.bin', 'vram-written-values.bin', 'vram-invalidated-mask.bin')),
                     'rng_draws_full_vs_light': [sum(1 for _ in open(os.path.join(fa, 'rng.jsonl'))), cp_info[cp]['rng_draws_before']]}
        ref['vs_full_reference'] = x
    if control_run:
        cev = read_jsonl(os.path.join(control_run, 'events.jsonl'))
        cr = [(e['restore']['state_1f89a8'], e['input_gate_open']) for e in cev if e['ev'] == 'enter' and e.get('name') == 'restore_input']
        lr = [(e['restore']['state_1f89a8'], e['input_gate_open']) for e in ev if e['ev'] == 'enter' and e.get('name') == 'restore_input']
        c = {'restore_input_sequence_equal_control': cr == lr, 'calls': [len(cr), len(lr)]}
        for name in ('empty-ready', 'second-title-return'):
            cs = read_jsonl(os.path.join(control_run, 'checkpoints', name, 'state.jsonl'))[0]
            ls_ = read_jsonl(os.path.join(a, 'checkpoints', name, 'state.jsonl'))[0]
            c[name] = {'rand_state_equal_control': cs['rand_state_214ec8_58'] == ls_['rand_state_214ec8_58'],
                       'resource_state_equal_control': all(retire.resource_equal(cs, ls_, tuple(k for k in retire.RESOURCE_STATE_KEYS
                                                                                            if k not in ('allocations', 'allocation_serials'))).values()),
                       'vram_fill00_equal_control': fsha(os.path.join(control_run, 'checkpoints', name, 'vram.bin'))
                       == fsha(os.path.join(a, 'checkpoints', name, 'vram.bin'))}
        ref['vs_minimal_control'] = c
    return ref

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('evidence')
    p.add_argument('--out', required=True)
    p.add_argument('--rng', required=True)
    p.add_argument('--retire-evidence', required=True)
    p.add_argument('--regress-retire')
    p.add_argument('--regress-title')
    p.add_argument('--regress-default')
    p.add_argument('--title-runs')
    p.add_argument('--startup-run')
    p.add_argument('--light', action='store_true', help='light reference (--restore-cancel --light-restore captures)')
    p.add_argument('--full-analysis', help='light: the full Restore analysis, for cross-checks')
    p.add_argument('--control', help='light: the minimal control run, for cross-checks')
    a = p.parse_args(argv)
    if os.path.exists(a.out):
        print('refusing existing output directory', file=sys.stderr)
        return 2
    OUT[0] = a.out
    try:
        if a.light:
            ref = analyse_light(a.evidence, a.out, a.rng, a.retire_evidence, a.full_analysis, a.control)
            with open(os.path.join(a.out, 'reference.json'), 'x') as f:
                json.dump(ref, f, indent=1)
                f.write('\n')
            for name, c in ref['checkpoints'].items():
                print(f'  {name}: vb={c["vb"]} t={c["t"]} rows={c["resource_events"]} draws={c["rng_draws_before"]}')
            print('readiness:', ref['card_readiness'], 'anchors:', ref['manager_frames']['anchors'])
            print('retire neutrality failures:', ref['retire_neutrality']['failures'])
            for k in ('vs_full_reference', 'vs_minimal_control'):
                if k in ref:
                    print(k, json.dumps(ref[k]))
            return 0
        ref = analyse(a.evidence, a.out, a.rng, a.retire_evidence)
        if a.regress_retire or a.regress_title or a.regress_default:
            need(not (a.regress_title or a.regress_default) or a.title_runs, '--regress-title/default need --title-runs')
            need(not a.regress_default or a.startup_run, '--regress-default needs --startup-run')
            ref['regression'] = regression(a.regress_retire, a.regress_title, a.regress_default, a.retire_evidence, a.title_runs, a.startup_run)
        with open(os.path.join(a.out, 'reference.json'), 'x') as f:
            json.dump(ref, f, indent=1)
            f.write('\n')
    except Fail as exc:
        print('FAIL:', exc, file=sys.stderr)
        return 1
    for name, c in ref['checkpoints'].items():
        line = f'  {name}: vb={c["vb"]} t={c["t"]} resource_events={c["resource_events"]} draws={c["rng_draws_before"]}'
        if 'state' in c:
            line += ' allocations=%d handles=%d effect106=%s vram_known=%d' % (
                c['state']['allocations'], c['state']['handles'], json.dumps(c['effect106']), c['vram']['known_nibbles_required'])
        print(line)
    print('retire neutrality failures:', ref['retire_neutrality']['failures'])
    print('glyph:', ref['glyph']['verdict'])
    if 'control_minimal' in ref:
        print('control: restore_input_sequence_equal=%s' % ref['control_minimal']['restore_input_sequence_equal'])
    if 'regression' in ref:
        for k, v in ref['regression'].items():
            if isinstance(v, dict) and all(isinstance(x, bool) for x in v.values()):
                print(k, 'all identical' if all(v.values()) else 'DIFFERENT: ' + json.dumps({x: y for x, y in v.items() if not y}))
    return 0

if __name__ == '__main__':
    sys.exit(main())
