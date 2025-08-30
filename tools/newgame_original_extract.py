#!/usr/bin/env python3
"""Extract the New Game -> name entry -> Chapter 1 original reference from `okage-startup-oracle
--newgame-chapter` captures.

usage: newgame_original_extract.py EVIDENCE_DIR --out ANALYSIS_DIR --rng RNG_JSONL
           --retire-evidence RETIRE_EVIDENCE_DIR [--regress-retire RUN] [--regress-title RUN] [--regress-default RUN]
           [--title-runs DIR] [--startup-run DIR]

EVIDENCE_DIR/runs/{fill00-a,fill00-b,fillff-a,fillff-b,control-minimal-fill00} (tools/newgame_capture.sh).
--retire-evidence: the pinned Retire title-retirement evidence (runs/newgame/* and analysis/newgame/*); the Newgame run must
reproduce it exactly up to the 0x124548 entry (retire-successor-entry).
--regress-retire/--regress-title/--regress-default: runs of the SAME oracle binary in `--title-retire newgame`, `--title`
and default mode, checked byte-identical (modulo documented host-timing fields) to the Retire newgame fill00-a, Title
fill00-a and Startup fill00-a captures.

Writes ANALYSIS_DIR (must not exist):
  reference.json            checkpoints (vblank, traversal t relative to the name-entry construction traversal S,
                            boundary), pad script and consumption, RNG provenance and sites, image counter, glyph
                            writers, disc reads, tails, determinism, Retire neutrality, minimal control, regression
  events-detail.jsonl       the ordered lifecycle timeline with source attribution
  name-frames.jsonl         one row per name-entry wrapper traversal (t = 0 is S)
  chapter-frames.jsonl      one row per 0x133a98 call (Chapter 1, phase 19)
  rng-detail.jsonl          every draw after the 0x100618 prefix (ra, frames, states, traversal)
  <checkpoint>/events.jsonl lifecycle prefix from cold boot (native lifecycle schema, Newgame vocabulary)
  <checkpoint>/state.json   allocator/handle/record/list state (coverage lifecycle "newgame")
  <checkpoint>/effects.json {"effect106": ...}
  <checkpoint>/rng.jsonl    every draw before the checkpoint
  <checkpoint>/session.json image counter 0x337500, command 0x24f740, published name +0x844, temporary bank hash,
                            fade 0x24eab0..bc, mode parent {phase +20, queue [+3c,+48,+50], current [+40,+44,+4c]}
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
import restore_original_extract as restore  # noqa: E402

RUNS = ('fill00-a', 'fill00-b', 'fillff-a', 'fillff-b')
CONTROL = 'control-minimal-fill00'
RETIRE_CPS = ('retire-title-constructed', 'retire-first-update', 'retire-stable-menu', 'retire-removal-request',
           'retire-resource-retirement')
CHECKPOINTS = ('retire-successor-entry', 'name-constructor-return', 'first-layout', 'name-edited', 'name-confirmed',
               'name-death', 'manager-free', 'command-published', 'resource-retirement', 'wrapper-free',
               'chapter-constructor-return', 'chapter-fade-request', 'chapter-mode-request',
               'chapter-resource-retirement', 'chapter-wrapper-free', 'field-constructor-entry')
CONTROL_CPS = ('retire-successor-entry', 'name-constructor-return', 'name-edited', 'name-confirmed',
               'chapter-constructor-return', 'field-constructor-entry')
ENTRY_TRIGGER = {'retire-title-constructed': None, 'retire-first-update': 'title_update', 'retire-stable-menu': 'title_update',
                 'retire-removal-request': 'effect_remove_request', 'retire-resource-retirement': 'mode8_destroy',
                 'retire-successor-entry': 'successor_construct', 'name-death': 'manager_destroy', 'manager-free': 'host_free',
                 'resource-retirement': 'wrapper_destroy', 'wrapper-free': 'host_free',
                 'chapter-resource-retirement': 'mode2_destroy', 'chapter-wrapper-free': 'host_free',
                 'field-constructor-entry': 'mode2_construct'}
RETURN_TRIGGER = {'name-constructor-return': 'successor_construct', 'first-layout': 'resource_tick',
                  'name-edited': 'name_input', 'name-confirmed': 'name_input', 'command-published': 'wrapper_tick',
                  'chapter-constructor-return': 'mode2_construct', 'chapter-fade-request': 'mode2_update',
                  'chapter-mode-request': 'mode2_update'}
MEMORY = 'host-uploads-known-draw-writes-unknown'
SCHEDULER_PC = '0x100618'
RNG_SITES = {'0x19c754': 'name-ui-glyph', '0x19cadc': 'name-ui-icon', '0x19cc80': 'name-ui-select'}
NAME_INPUT = (0x11a1c8, 0x11b2f0)  # pad reads attributed to 0x11a1c8 by call site

Fail = title.Fail
need = title.need
fsha = title.fsha
read_jsonl = title.read_jsonl
jl = title.jl
write_x = retire.write_x
effect_state = retire.effect_state

def timeline(events):
    enters = {}
    out, detail = [], []
    live = set()
    archive = xmd_path = None
    counts = collections.Counter()
    title_n = None
    last_sub = None
    trav = 0
    successor = False

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
        need(o['owner_mode'] in (8, 9, 2) and o['owner_selector'] >= 0, f'unmodelled owner {o}')
        node = {'mode9_10': 'mode%d' % o['owner_mode']}.get(o['node_kind'], o['node_kind'])
        if op == 'wrapper-free':
            need(node in ('resource', 'mode8', 'mode9', 'mode2', 'manager'), f'scheduler freed an unmodelled node {o}')
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
            elif n == 'mode2_destroy':
                add({'kind': 'mode', 'op': 'destroy', 'mode': 2, 'selector': e['mode2']['dest_28']}, e)
            elif n == 'mode2_construct':
                cmd = e['game']['command_24f740']
                add({'kind': 'phase', 'name': 'mode2-0x133830-entry-command%d' % cmd[0]
                     + ('-chapter%d' % cmd[1] if cmd[0] == 14 else '')}, e)
            elif n == 'mode_request':
                add({'kind': 'mode', 'op': 'request', 'mode': e['mode'], 'selector': e['selector'], 'arg': e['arg3']}, e)
            elif n == 'mode_result_set' and successor:
                add({'kind': 'mode', 'op': 'command', 'command': e['command']}, e)
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
            elif n == 'window_destroy':
                add({'kind': 'ui', 'op': 'window-death', 'window': e['value']}, e)
            elif n == 'manager_destroy':
                add({'kind': 'ui', 'op': 'manager-destroy', 'mode': 9}, e)
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
                add({'kind': 'ui', 'op': 'manager-create', 'mode': 9, 'windows': e['windows']}, e, ent)
                for h in e['window_handles']:
                    add({'kind': 'ui', 'op': 'window-create', 'window': h}, e, ent)
        elif ev == 'prefix_end':
            add({'kind': 'phase', 'name': 'prefix-end-0x100618'}, e)
        elif ev == 'checkpoint':
            add({'kind': 'phase', 'name': 'checkpoint-' + e['name']}, e)
            if e['name'] == 'retire-successor-entry':
                successor = True
        elif ev == 'stop':
            add({'kind': 'phase', 'name': 'stop-' + e['kind']}, e)
    need(all(a['oracle_i'] <= b['oracle_i'] for a, b in zip(detail, detail[1:])), 'timeline not in source order')
    return out, detail, counts

def checkpoint_boundaries(ev):
    out = {}
    by_i = {e['i']: e for e in ev}
    for e in ev:
        if e['ev'] != 'checkpoint':
            continue
        name = e['name']
        need(name not in out, 'checkpoint repeated: ' + name)
        k = e['i'] - 1
        while by_i[k]['ev'] == 'pad_script':
            k -= 1
        prev = by_i[k]
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

def rng_draws(ev, rng, successor_i):
    """Retire draw extraction (every draw is a hooked rand() entry with its observed return); draws after the Retire
    successor entry are labelled by their call site."""
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
        x['provenance'] = 'observed'
        if x['oracle_i'] > successor_i:
            need(x['ra'] in RNG_SITES, f'draw at an unmodelled site {x["ra"]}')
            d['phase'] = RNG_SITES[x['ra']]
            x['phase'] = d['phase']
    return draws, detail

def session_state(fs):
    g = fs['game']
    mp = fs['mode_parent_fields']
    return {'image_counter': g['image_counter_337500'], 'command': g['command_24f740'], 'name_844': g['name_844'],
            'temp_10_sha256': g['temp_10_sha256'], 'fade': g['fade'],
            'mode_parent': {'phase': mp['phase_20'], 'queue': [mp['request_3c'], mp['w48'], mp['w50']],
                            'current': [mp['current_40'], mp['w44'], mp['w4c']]}}

def in_name_input(ra):
    return ra is not None and NAME_INPUT[0] <= int(ra, 16) < NAME_INPUT[1]

def name_frames(ev, trav_of, s_trav, draw_detail, successor_i):
    """One row per traversal in which the mode-9 wrapper ran (0x124630), t = 0 at S: the 0x11a1c8 observations of that
    traversal (phase at entry, phase/length/selection/Yes-No/name at return, the pad words it read) and its draws."""
    rows = {}
    cur = None
    for e in ev:
        if e['i'] <= successor_i:
            continue
        t = trav_of[e['i']] - s_trav
        if e['ev'] == 'enter' and e['name'] == 'wrapper_tick':
            rows.setdefault(t, {'t': t, 'manager_ran': False})
        elif e['ev'] == 'enter' and e['name'] == 'name_input':
            r = rows.setdefault(t, {'t': t, 'manager_ran': False})
            need(not r['manager_ran'], f'two 0x11a1c8 calls in traversal {t}')
            r.update({'manager_ran': True, 'phase_in': e['name_entry']['phase'], 'pressed': None, 'repeat': None})
            cur = r
        elif e['ev'] == 'exit' and e['name'] in ('pad_pressed', 'pad_repeat') and cur is not None \
                and in_name_input(e['caller_ra']):
            k = 'pressed' if e['name'] == 'pad_pressed' else 'repeat'
            need(cur[k] is None, f'two {k} reads in one 0x11a1c8 call')
            cur[k] = e['word']
        elif e['ev'] == 'exit' and e['name'] == 'name_input':
            n = e['name_entry']
            cur.update({'phase_out': n['phase'], 'length_out': n['length_a8'], 'selection_out': n['selection_cc'],
                        'yes_no_out': n['yes_no_d8'], 'name_out': e['game']['name_844']})
            cur = None
    for d in draw_detail:
        if d['oracle_i'] > successor_i and d['trav'] - s_trav in rows:
            r = rows[d['trav'] - s_trav]
            r['draws'] = r.get('draws', 0) + 1
    out = []
    for t in sorted(rows):
        r = rows[t]
        m = r['manager_ran']
        out.append({'t': t, 'manager_ran': m, 'phase_in': r.get('phase_in'), 'phase_out': r.get('phase_out'),
                    'length_out': r.get('length_out'), 'selection_out': r.get('selection_out'),
                    'yes_no_out': r.get('yes_no_out'), 'pressed': r.get('pressed') if m else None,
                    'repeat': r.get('repeat') if m else None, 'draws': r.get('draws', 0),
                    'name_out': r.get('name_out') if m else None})
    return out

def chapter_frames(ev, trav_of, s_trav, draw_detail, successor_i):
    """One row per 0x133a98 call: t, the chapter counter (+0x38) and phase (+0x48) at its return, the fade
    current/target (0x24eab0/b4) and image counter (0x337500) at its entry, the fade target and command at its return,
    and the draws of that traversal."""
    out = []
    enters = {}
    draws = collections.Counter(d['trav'] - s_trav for d in draw_detail if d['oracle_i'] > successor_i)
    for e in ev:
        if e['i'] <= successor_i:
            continue
        if e['ev'] == 'enter' and e['name'] == 'mode2_update':
            enters[e['i']] = e
        elif e['ev'] == 'exit' and e['name'] == 'mode2_update':
            a = enters[e['enter']]
            t = trav_of[a['i']] - s_trav
            out.append({'t': t, 'counter_out': e['mode2']['counter_38'], 'phase_out': e['mode2']['phase_48'],
                        'fade_current_in': a['game']['fade']['current'], 'fade_target_in': a['game']['fade']['target'],
                        'fade_target_out': e['game']['fade']['target'], 'command_out': e['game']['command_24f740'],
                        'image_counter_in': a['game']['image_counter_337500'], 'draws': draws.get(t, 0)})
    return out

def analyse(evidence, out, rng, retire_evidence):
    runs = {r: os.path.join(evidence, 'runs', r) for r in RUNS}
    for r, p in runs.items():
        need(os.path.isdir(p), f'missing run {p}')
    os.makedirs(out)
    det = retire.determinism(runs, RETIRE_CPS + CHECKPOINTS)
    need(all(v['identical'] for k, v in det.items() if isinstance(v, dict) and 'identical' in v),
         'runs are not deterministic: ' + json.dumps({k: v.get('identical') for k, v in det.items() if isinstance(v, dict)}))
    a = runs['fill00-a']
    ev = read_jsonl(os.path.join(a, 'events.jsonl'))
    meta = json.load(open(os.path.join(a, 'run-meta.json')))
    need(meta.get('newgame_chapter') is True and meta['pad_step'] == 23 and meta['missing_checkpoints'] == [],
         'run-meta does not describe a completed New Game -> Chapter 1 script')
    need(ev[-1]['ev'] == 'stop' and ev[-1]['kind'] == 'field_constructor_entry', 'capture does not end at the second 0x133830 entry')
    need(meta['checkpoints'] == list(RETIRE_CPS) + list(CHECKPOINTS), f'checkpoint order {meta["checkpoints"]}')
    tl, detail, counts = timeline(ev)
    for r in RUNS[1:]:
        need(timeline(read_jsonl(os.path.join(runs[r], 'events.jsonl')))[0] == tl, f'timeline differs in {r}')
    cps = checkpoint_boundaries(ev)
    need(tuple(cps) == RETIRE_CPS + CHECKPOINTS, f'checkpoints {list(cps)}')
    successor_i = cps['retire-successor-entry'][1]
    rng_rows = read_jsonl(rng)
    draws, draw_detail = rng_draws(ev, rng_rows, successor_i)
    ptrs = restore.table_ptrs(ev)
    need(len(ptrs) == 1, 'expected one selector-1 title construction')

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
        pre = restore.prefix(detail, tl, boundary)
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
        st = restore.native_state(fs, name, ptrs)
        st['coverage'] = {'lifecycle': 'newgame', 'checkpoint': name, 'memory': MEMORY}
        write_x(os.path.join(d, 'state.json'), json.dumps(st, separators=(',', ':')) + '\n')
        fx = {'effect106': effect_state(fs['title_effect']) if fs['title_effect_slot'] >= 0 else None}
        write_x(os.path.join(d, 'effects.json'), json.dumps(fx, separators=(',', ':')) + '\n')
        write_x(os.path.join(d, 'rng.jsonl'), jl(draws[:info['rng_draws_before']]))
        ss = session_state(fs)
        write_x(os.path.join(d, 'session.json'), json.dumps(ss, separators=(',', ':')) + '\n')
        cpd = {r: os.path.join(runs[r], 'checkpoints', name) for r in RUNS}
        info['vram'] = retire.vram_analysis(os.path.join(cpd['fill00-a'], 'vram.bin'), os.path.join(cpd['fillff-a'], 'vram.bin'),
                                         os.path.join(cpd['fill00-a'], 'vram-last-writer.bin'),
                                         os.path.join(cpd['fill00-a'], 'vram-upload-ever.bin'), d)
        info['state'] = {'allocations': len(st['allocations']), 'handles': len(st['handles']), 'records': len(st['records']),
                         'hash': len(st['hash']), 'image_list': st['image_list'], 'materials': len(st['materials']),
                         'available': st['available'], 'reserved': len(st['reserved']),
                         'hash_tombstones': sum(1 for _, v in st['hash'] if v == -1)}
        info['effect106'] = fx['effect106']
        info['session'] = ss
        info['theme_words_2d9a60'] = fs['theme_words_2d9a60']
    need(all(cp_info[x]['oracle_i'] < cp_info[y]['oracle_i'] for x, y in zip(CHECKPOINTS, CHECKPOINTS[1:])), 'checkpoint order')

    nf = name_frames(ev, trav_of, s_trav, draw_detail, successor_i)
    cf = chapter_frames(ev, trav_of, s_trav, draw_detail, successor_i)
    write_x(os.path.join(out, 'name-frames.jsonl'), jl(nf))
    write_x(os.path.join(out, 'chapter-frames.jsonl'), jl(cf))
    write_x(os.path.join(out, 'rng-detail.jsonl'), jl(draw_detail[len(rng_rows):]))
    ref = {'schema': 'okage.newgame-original-reference.v1', 'route': 'newgame-chapter',
           'checkpoint_rule': 'entry checkpoints precede every event of that call (boundary = entry index, exclusive); '
                              'return checkpoints follow every event of that call (boundary = exit index + 1)',
           'construction_traversal_S': s_trav, 'checkpoints': cp_info, 'timeline_counts': dict(sorted(counts.items()))}
    ref.update(route_facts(ev, nf, cf, draw_detail, cps, trav_of, s_trav, successor_i, rng_rows))
    ref['determinism'] = det
    ref['retire_neutrality'] = retire_neutrality(a, ev, tl, detail, cps, out, retire_evidence, runs)
    ctl = os.path.join(evidence, 'runs', CONTROL)
    if os.path.isdir(ctl):
        ref['control_minimal'] = control(ctl, a, ev)
    return ref

def route_facts(ev, nf, cf, draw_detail, cps, trav_of, s_trav, successor_i, rng_rows):
    post = [e for e in ev if e['i'] > successor_i]
    t = lambda e: trav_of[e['i']] - s_trav  # noqa: E731
    ent = lambda n: [e for e in post if e['ev'] == 'enter' and e['name'] == n]  # noqa: E731
    f = {}
    f['pad_script'] = [{k: e[k] for k in ('i', 'vb', 'step', 'buttons', 'why')} | {'t': t(e) if e['i'] > successor_i else None}
                       for e in ev if e['ev'] == 'pad_script']
    f['name_pad_consumed'] = [{'t': r['t'], 'pressed': r['pressed'], 'repeat': r['repeat'], 'phase_in': r['phase_in'],
                               'phase_out': r['phase_out']} for r in nf if r['manager_ran'] and (r['pressed'] or r['repeat'])]
    post_draws = [d for d in draw_detail if d['oracle_i'] > successor_i]
    f['rng'] = {
        'total_draws': len(draw_detail), 'prefix_draws_rng': len(rng_rows), 'post_successor_draws': len(post_draws),
        'provenance': 'every draw is a hooked 0x1f14b0 entry with its observed return (state and value); none derived',
        'sites': dict(sorted(collections.Counter(d['phase'] for d in post_draws).items())),
        'traversal_range': [min(d['trav'] for d in post_draws) - s_trav, max(d['trav'] for d in post_draws) - s_trav]
        if post_draws else None,
        'name_frame_draws': sum(r['draws'] for r in nf), 'chapter_frame_draws': sum(r['draws'] for r in cf)}
    need(f['rng']['name_frame_draws'] + f['rng']['chapter_frame_draws'] == len(post_draws),
         'post-successor draws outside the name/chapter traversals')
    steps = ent('image_counter_step')
    f['image_counter'] = {
        'at_first_name_input': read_counter(post, 'name_input'),
        'steps_after_successor': len(steps), 'step_traversals': [t(steps[0]), t(steps[-1])] if steps else None,
        'step_callers': dict(collections.Counter(e['parent'] for e in steps)),
        'name_input_values': sorted({e['game']['image_counter_337500'] for e in ent('name_input')}),
        'note': '1b3450 (the only 0x337500 increment) is reached only through 100f90 -> 100800 -> 1b5ee0 when the '
                'resource +0x24 flag is set; 1009e0 sets it on a successful image-list load (Chapter 1 only).'}
    f['glyph'] = {'cache_transfer_0x1976f0': len(ent('glyph_cache_transfer')), 'cache_lookup_0x197c68': len(ent('glyph_cache_lookup')),
                  'glyph_path_0x195ff0': len(ent('glyph_path_195ff0')),
                  'verdict': 'no glyph-cache writer or lookup reached after the Retire successor entry on this route'
                  if not (ent('glyph_cache_transfer') or ent('glyph_cache_lookup') or ent('glyph_path_195ff0')) else 'reached'}
    reads = []
    for e in post:
        if e['ev'] == 'exit' and e['name'] == 'file_read_raw':
            reads.append({'t': t(e), 'file': e['file'], 'bytes': int(e['bytes_read']), 'sha256': e['sha256']})
    f['disc_reads_after_successor'] = reads
    theme = ent('theme_end_12de98')
    f['tails'] = {'theme_end_t': t(theme[0]) if theme else None, 'theme_end_calls': len(theme),
                  'effect_reclaims': [{'t': t(e), 'slot': e['job_slot']} for e in ent('effect_free') if e.get('job_slot', -1) >= 0]}
    f['commands'] = [{'t': t(e), 'command': e['command']} for e in ent('mode_result_set')]
    f['mode_requests'] = [{'t': t(e), 'mode': e['mode'], 'selector': e['selector'], 'arg': e['arg3']} for e in ent('mode_request')]
    f['fade_requests'] = [{'t': t(e), 'f12_bits': e['f12_bits'], 'caller_ra': e['ra']} for e in ent('fade_request')]
    f['temp_clear'] = [{'t': t(e), 'name_844': e['game']['name_844'], 'temp_10_sha256_before': e['game']['temp_10_sha256']}
                       for e in ent('temp_clear')]
    first_update = [r for r in cf]
    f['chapter'] = {'updates': len(first_update), 'first_t': first_update[0]['t'] if first_update else None,
                    'counter_240_t': next((r['t'] for r in cf if r['counter_out'] == 240), None),
                    'command_clear_t': next((r['t'] for r in cf if r['command_out'] == [0, 0, 0]), None)}
    return f

def read_counter(post, name):
    for e in post:
        if e['ev'] == 'enter' and e['name'] == name:
            return e['game']['image_counter_337500']
    return None

def retire_neutrality(a, ev, tl, detail, cps, out, retire_evidence, runs):
    """The Newgame run must equal the pinned Retire newgame capture up to the 0x124548 entry."""
    res = {}
    retire_run = os.path.join(retire_evidence, 'runs', 'newgame', 'fill00-a')
    retire_an = os.path.join(retire_evidence, 'analysis', 'newgame', 'successor-entry')
    boundary = cps['retire-successor-entry'][1]
    mine = [line for line in open(os.path.join(a, 'events.jsonl')) if json.loads(line)['i'] <= boundary + 1]
    theirs = [line for line in open(os.path.join(retire_run, 'events.jsonl'))][:len(mine)]
    rename = lambda s: s.replace('"name":"retire-', '"name":"')  # noqa: E731
    res['raw_events_through_successor_checkpoint_equal_modulo_retire_prefix'] = [rename(x) for x in mine] == theirs
    res['raw_events_compared'] = len(mine)
    pre = restore.prefix(detail, tl, boundary)
    retire_rows = read_jsonl(os.path.join(retire_an, 'events.jsonl'))
    res['lifecycle_equal_retire_successor_entry'] = restore.retire_view(pre) == [
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
        td = os.path.join(retire_evidence, 'runs', 'newgame', r, 'checkpoints', 'successor-entry')
        mine_s = retire.strip_fill(read_jsonl(os.path.join(cd, 'state.jsonl'))[0])
        their_s = retire.strip_fill(read_jsonl(os.path.join(td, 'state.jsonl'))[0])
        res[f'{r}: state.jsonl (fill fields removed; Newgame-only keys ignored)'] = {k: mine_s[k] for k in their_s if k in mine_s} == their_s \
            and set(their_s) <= set(mine_s)
        for n in ('vram.bin', 'vram-last-writer.bin', 'vram-upload-ever.bin'):
            res[f'{r}: {n}'] = fsha(os.path.join(cd, n)) == fsha(os.path.join(td, n))
    res['failures'] = [k for k, v in res.items() if v is False]
    return res

def control(ctl, a, ev):
    """The minimal-hook control (title update, 0x124548, 0x11a1c8, 0x133830 and their returns only)."""
    cev = read_jsonl(os.path.join(ctl, 'events.jsonl'))
    cmeta = json.load(open(os.path.join(ctl, 'run-meta.json')))
    calls = lambda evs, upto: sum(1 for e in evs if e['ev'] == 'enter' and e.get('name') == 'name_input' and e['i'] <= upto)  # noqa: E731
    cck = {e['name']: e for e in cev if e['ev'] == 'checkpoint'}
    hck = {e['name']: e for e in ev if e['ev'] == 'checkpoint'}
    out = {'vblanks': cmeta['vblanks'], 'checkpoints': cmeta['checkpoints'], 'missing': cmeta['missing_checkpoints']}
    cmp = {}
    for name in CONTROL_CPS:
        cs = read_jsonl(os.path.join(ctl, 'checkpoints', name, 'state.jsonl'))[0]
        hs = read_jsonl(os.path.join(a, 'checkpoints', name, 'state.jsonl'))[0]
        cmp[name] = {'resource_state_equal': retire.resource_equal(cs, hs, tuple(k for k in retire.RESOURCE_STATE_KEYS
                                                                                 if k not in ('allocations', 'allocation_serials'))),
                     'session_state_equal': session_state(cs) == session_state(hs),
                     'rand_state_equal': cck[name]['rand_state_214ec8_58'] == hck[name]['rand_state_214ec8_58'],
                     'name_input_calls_before_equal': calls(cev, cck[name]['i']) == calls(ev, hck[name]['i']),
                     'name_input_calls_before': [calls(cev, cck[name]['i']), calls(ev, hck[name]['i'])],
                     'vram_fill00_equal': fsha(os.path.join(ctl, 'checkpoints', name, 'vram.bin'))
                     == fsha(os.path.join(a, 'checkpoints', name, 'vram.bin')),
                     'vb_control': cck[name]['vb'], 'vb_hooked': hck[name]['vb']}
    out['checkpoints_compared'] = cmp
    seq = lambda evs: [(e['name_entry']['phase'], e['name_entry']['length_a8'], e['name_entry']['selection_cc'],  # noqa: E731
                        e['name_entry']['yes_no_d8'], e['game']['image_counter_337500'])
                       for e in evs if e['ev'] == 'exit' and e.get('name') == 'name_input']
    cr, hr = seq(cev), seq(ev)
    out['name_input_sequence_equal'] = cr == hr
    out['name_input_calls'] = [len(cr), len(hr)]
    out['first_difference'] = next((i for i, (x, y) in enumerate(zip(cr, hr)) if x != y), None)
    out['final_vram_equal'] = fsha(os.path.join(ctl, 'vram.bin')) == fsha(os.path.join(a, 'vram.bin'))
    out['final_session_equal'] = session_state(read_jsonl(os.path.join(ctl, 'final-state.jsonl'))[0]) == \
        session_state(read_jsonl(os.path.join(a, 'final-state.jsonl'))[0])
    out['pad_host_equal_modulo_vb'] = [{k: r[k] for k in ('buttons', 'play_mask')} for r in read_jsonl(os.path.join(ctl, 'pad-host.jsonl'))] \
        == [{k: r[k] for k in ('buttons', 'play_mask')} for r in read_jsonl(os.path.join(a, 'pad-host.jsonl'))]
    out['failures'] = sorted([f'{n}:{k}' for n, c in cmp.items() for k, v in c.items()
                              if (v is False) or (isinstance(v, dict) and not all(v.values()))]
                             + [k for k in ('name_input_sequence_equal', 'final_vram_equal', 'final_session_equal',
                                            'pad_host_equal_modulo_vb') if not out[k]])
    out['note'] = ('the control hooks only what the scripts and stop need; it has no traversal counter, so the name-input '
                   'call count to each checkpoint stands in for traversal timing (one 0x11a1c8 call per traversal)')
    return out

def regression(retire_run, title_run, default_run, retire_evidence, title_runs, startup_run):
    out = {}
    if retire_run:
        ref = os.path.join(retire_evidence, 'runs', 'newgame', 'fill00-a')
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
        out['retire_newgame_mode_vs_retire_fill00_a'] = r
    if title_run or default_run:
        out.update(retire.regression(title_run, default_run, title_runs, startup_run))
    return out

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('evidence')
    ap.add_argument('--out', required=True)
    ap.add_argument('--rng', required=True)
    ap.add_argument('--retire-evidence', required=True)
    ap.add_argument('--regress-retire')
    ap.add_argument('--regress-title')
    ap.add_argument('--regress-default')
    ap.add_argument('--title-runs')
    ap.add_argument('--startup-run')
    a = ap.parse_args(argv)
    try:
        if os.path.exists(a.out):
            raise Fail('output directory exists')
        ref = analyse(a.evidence, a.out, a.rng, a.retire_evidence)
        if a.regress_retire or a.regress_title or a.regress_default:
            ref['regression'] = regression(a.regress_retire, a.regress_title, a.regress_default, a.retire_evidence,
                                           a.title_runs, a.startup_run)
        problems = list(ref['retire_neutrality']['failures'])
        if 'control_minimal' in ref:
            problems += ['control:' + x for x in ref['control_minimal']['failures']]
        for k, v in ref.get('regression', {}).items():
            if k.endswith('_keys_added_since_startup_binary'):
                continue
            problems += [f'regression {k}: {n}' for n, ok in v.items() if ok is False]
        ref['problems'] = problems
        write_x(os.path.join(a.out, 'reference.json'), json.dumps(ref, indent=1, sort_keys=True) + '\n')
        print(f'Newgame reference: {len(ref["checkpoints"])} checkpoints, {ref["rng"]["total_draws"]} draws '
              f'({ref["rng"]["post_successor_draws"]} after the successor entry), problems: {problems or "none"}')
        return 1 if problems else 0
    except Fail as e:
        print('FAIL:', e, file=sys.stderr)
        return 1

if __name__ == '__main__':
    sys.exit(main())
