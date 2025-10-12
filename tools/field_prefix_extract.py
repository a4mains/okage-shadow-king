#!/usr/bin/env python3
"""Extract the Field ordinary-field common-prefix original reference from `okage-startup-oracle --newgame-chapter
--field-prefix` captures.

usage: field_prefix_extract.py EVIDENCE_DIR --out ANALYSIS_DIR --rng RNG_JSONL --newgame-analysis NEWGAME_ANALYSIS
           [--regress-newgame RUN]

EVIDENCE_DIR/runs/{fill00-a,fill00-b,fillff-a,fillff-b,control-minimal-fill00} (tools/newgame_capture.sh with
mode `--newgame-chapter+--field-prefix`, the control with `--newgame-chapter+--minimal+--field-prefix`).
--newgame-analysis: the pinned Newgame analysis (docs/newgame-original-manifest.json). The Field capture observes exactly the
Newgame hooks before the second 0x133830 entry, so every Newgame checkpoint file regenerated from it must equal the pinned one.
--regress-newgame: a `--newgame-chapter` run (no --field-prefix) of the SAME oracle binary; its events must equal the Field
run's events up to the Newgame stop (the oracle change is neutral for the Newgame mode).

Writes ANALYSIS_DIR (must not exist):
  reference.json   checkpoints (oracle index, vblank, traversal), scheduling record, RNG provenance, determinism,
                   Newgame neutrality, the bracketed 0x131e7c proof, minimal control, missing observations
  events-detail.jsonl  the ordered lifecycle timeline (Newgame vocabulary plus {"kind":"field"} rows) with attribution
  <checkpoint>/    for the seven Field checkpoints: events.jsonl, state.json, effects.json, rng.jsonl, session.json,
                   field.json, vram-written-mask.bin, vram-written-values.bin, vram-invalidated-mask.bin
The last checkpoint, ui-prefix-published, is DERIVED: the 0x131e74 block-start snapshot (observed) plus the single
store of 0x131e7c (manager+0x34 = 1), proved by the next block-start snapshot (only that word and the documented
0x131e80..0x131ec0 stores differ in EE RAM; GS, RNG and resource state unchanged).
Reads only; exits 1 if the runs disagree or a cross-check fails. Research evidence only; never product input.
"""
import argparse
import collections
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import retire_title_original_extract as retire  # noqa: E402
import restore_original_extract as restore  # noqa: E402
import newgame_original_extract as newgame  # noqa: E402

RUNS = newgame.RUNS
CONTROL = newgame.CONTROL
RETIRE_CPS = newgame.RETIRE_CPS
NEWGAME_CPS = newgame.CHECKPOINTS
# Source checkpoints compared with the native Session (the last one derived, see above).
CHECKPOINTS = ('field-constructor-return', 'worker-entry', 'atlas-loaded', 'texenv-bound', 'texenv-loaded', 'ruw-loaded',
               'ui-prefix-published')
RAW_CPS = CHECKPOINTS[:-1] + ('ui-publish-pre', 'ui-publish-post-block')
CONTROL_CPS = ('field-constructor-return', 'worker-entry', 'ui-publish-pre', 'ui-publish-post-block')
# RETURN checkpoints follow every event of the call (boundary = exit index + 1); worker-entry precedes every event of
# 0x132688 (boundary = its entry index).
RETURN_TRIGGER = {'field-constructor-return': ('mode2_construct', None), 'atlas-loaded': ('atlas_load_psm', '0x131d14'),
                  'texenv-bound': ('env_bind', '0x100cc0'), 'texenv-loaded': ('texenv_load', '0x131e40'),
                  'ruw-loaded': ('ruw_load', '0x131e6c')}
MEMORY = newgame.MEMORY
LIFECYCLE = 'field-prefix'
PUBLISH_BLOCK, NEXT_A, NEXT_B = 0x131e74, 0x131ec4, 0x131ec8
BLOCK_STORES = {0x80: ('s6', 0x0), 0x408: ('s6', 0x4), 0x40c: ('s6', 0x8), 0x410: ('s6', 0xc), 0x400: ('s6', 0x28),
                0x404: ('s6', 0x2c), 0x42c: ('const', 0x21), 0x414: ('const', 0x11), 0x34: ('const', 1)}
UNMODELED = ('0x1b7eb0(10) -> 0x337e44', '0x135aa0 event-system globals', '0x1315d0 handler registrations',
             '0x1b8348(1) -> 0x337d78/0x337d7c/0x337d80', '0x1257b8 scene lookup -> s6', '0x131da0 manager+0x1a8..+0x2a7 = 1',
             '0x1bccf0/0x1bcc80 main.xpf mount/unmount', '0x1c0930 directory prefix',
             'manager+0x38 counter (scheduling; retained in reference.json)',
             '0x332b98 RUW ready word (1 at entry and at the stop)', '0x3abb14 reserve class switch by the job windows')

Fail = newgame.Fail
need = newgame.need
fsha = newgame.fsha
read_jsonl = newgame.read_jsonl
jl = newgame.jl
write_x = newgame.write_x

def field_rows(events):
    """Field {"kind":"field"} rows (native vocabulary) with their oracle index, in source order."""
    out = []
    enters = {}
    armed = False
    ruw_file = None
    for e in events:
        ev = e['ev']
        if ev == 'field_hooks_armed':
            armed = True
            continue
        if not armed:
            continue
        if ev == 'enter':
            enters[e['i']] = e
            n, a = e['name'], e.get('args', [])
            if n == 'switch_1bafd0':
                out.append((e['i'], {'kind': 'field', 'op': 'switch-337e70', 'value': int(a[0], 16)}))
            elif n == 'job_enqueue':
                need(int(a[0], 16) == 0x132688, 'the second 0x133830 enqueued another job')
                out.append((e['i'], {'kind': 'field', 'op': 'job-enqueue', 'job': '0x132688', 'scene': int(a[2], 16),
                                     'arg': int(a[3], 16)}))
            elif n == 'field_worker':
                out.append((e['i'], {'kind': 'field', 'op': 'worker-entry'}))
            elif n == 'clut_class_set':
                out.append((e['i'], {'kind': 'field', 'op': 'clut-class', 'value': int(a[0], 16)}))
            elif n == 'lower_enable_set' and e.get('parent') == 'field_prefix':
                out.append((e['i'], {'kind': 'field', 'op': 'lower-case', 'value': int(a[0], 16)}))
        elif ev == 'exit':
            ent = enters.get(e['enter'])
            n = e['name']
            if ent is None:
                continue
            if n == 'node_create' and ent.get('ra') == '0x1338cc':
                need(int(ent['args'][1], 16) == 0x221980, 'the second 0x133830 created another node type')
                out.append((e['i'], {'kind': 'field', 'op': 'resource-create'}))
            elif n == 'atlas_load_psm' and ent.get('ra') == '0x131d14':
                out.append((e['i'], {'kind': 'field', 'op': 'atlas', 'handle': int(e['v0'], 16)}))
            elif n == 'xim_load' and ent.get('ra') == '0x100ca0':
                out.append((e['i'], {'kind': 'field', 'op': 'texenv', 'handle': int(e['v0'], 16)}))
            elif n == 'env_bind' and ent.get('ra') == '0x100cc0':
                out.append((e['i'], {'kind': 'field', 'op': 'env-bind', 'handle': int(ent['args'][0], 16), '_state_at': e['i'] + 1}))
            elif n == 'file_read_raw' and e.get('file') == 'fld.ruw':
                ruw_file = (e['bytes_read'], e['sha256'])
            elif n == 'ruw_load' and ent.get('ra') == '0x131e6c':
                need(ruw_file is not None and int(ent['args'][1], 16) == 0, 'fld.ruw read/slot not observed')
                out.append((e['i'], {'kind': 'field', 'op': 'ruw', 'slot': 0, 'bytes': ruw_file[0], 'sha256': ruw_file[1]}))
    return out

def tex0_hex(f):
    return '0x%08x%08x' % (int(f['tex0_332430'][1], 16), int(f['tex0_332430'][0], 16))

def field_json(f, rows_before, ram=None):
    """The native field.json schema from the oracle's field dump (and, for the derived checkpoint, EE RAM)."""
    q = [r for r in rows_before if r['kind'] == 'field']
    enq = [r for r in q if r['op'] == 'job-enqueue']
    ruw = [r for r in q if r['op'] == 'ruw']
    need(len(enq) <= 1 and len(ruw) <= 1, 'repeated enqueue/RUW rows')
    live = f['ruw_slots_live']
    need(live[1:] == [0] * 15 and (live[0] == 1) == bool(ruw), 'RUW slot liveness does not follow the observed load')
    out = {'resource_created': int(f['resource_3c'], 16) != 0, 'switch_337e70': int(f['w337e70'], 16),
           'job_queued': bool(enq), 'scene': enq[0]['scene'] if enq else 0, 'entrance': enq[0]['arg'] if enq else 0,
           'w30': f['w30'], 'w34': f['w34'], 'manager_60': f['w60'],
           'resource_48': f['resource_48'] if 'resource_48' in f else -1,
           'atlas_global_24f730': f['atlas_global_24f730'], 'clut_class_2138c4': int(f['clut_class_2138c4'], 16),
           'lower_1fe30c': int(f['lower_1fe30c'], 16), 'cache_1fe310': int(f['cache_1fe310'], 16),
           'tex0_332430': tex0_hex(f), 'region_332450': [int(x, 16) for x in f['env_332450']],
           'ruw_slot0_live': bool(live[0]), 'ruw_bytes': ruw[0]['bytes'] if ruw else 0,
           'ruw_sha256': ruw[0]['sha256'] if ruw else None, 'reserve_class_3abb14': int(f['reserve_3abb14'], 16)}
    return out

def class_free_state(fs):
    """The job dispatcher (0x178950 -> 0x1cbdd8) selects reserve class 1 around every 0x132688 window; the class-1
    reserved queue is empty here, so the state is the class-0 schema with the class reported in field.json."""
    need(fs['reserved1'] == [] and fs['reserved'] == [],
         'reserve queues must both be empty for the bounded class normalization')
    out = dict(fs)
    out['reserve_class'] = 0
    return out

def word(ram, a):
    return struct.unpack_from('<I', ram, a & 0x1ffffff)[0]

def publish_proof(pre_dir, post_dir, pre_ev, post_ev):
    """Bracketed 0x131e7c proof from the two exact block-start snapshots."""
    pre = open(os.path.join(pre_dir, 'ee-ram-32mb.bin'), 'rb').read()
    post = open(os.path.join(post_dir, 'ee-ram-32mb.bin'), 'rb').read()
    need(len(pre) == len(post) == 0x2000000, 'EE RAM snapshots must be 32 MiB')
    need(pre_ev['pc'] == hex(PUBLISH_BLOCK) and post_ev['pc'] in (hex(NEXT_A), hex(NEXT_B)), 'bracket PCs')
    m, s6 = int(pre_ev['s4'], 16), int(pre_ev['s6'], 16)
    need(post_ev['s4'] == pre_ev['s4'] and pre_ev['sp'] == post_ev['sp'], 'bracket is not one 0x131d28 frame')
    expected = {}
    for off, (src, v) in BLOCK_STORES.items():
        expected[m + off] = word(pre, s6 + v) if src == 's6' else v
    changed = set()
    for k in range(0, 0x2000000, 4):
        if pre[k:k + 4] != post[k:k + 4]:
            changed.add(k)
    stores = {}
    for a, v in expected.items():
        stores[hex(a - m)] = {'before': word(pre, a), 'after': word(post, a), 'expected': v, 'ok': word(post, a) == v}
    need(word(pre, m + 0x34) == 0 and word(post, m + 0x34) == 1, 'manager+0x34 is not 0 -> 1 across the bracket')
    need(all(s['ok'] for s in stores.values()), f'0x131e74 block stores differ: {stores}')
    other = sorted(changed - set(expected))
    need(not other, f'unexplained RAM writes across publication bracket: {[hex(a) for a in other[:64]]}')
    return {'manager': hex(m), 's6': hex(s6), 'pre_pc': pre_ev['pc'], 'post_pc': post_ev['pc'], 'stores': stores,
            'changed_words_total': len(changed), 'changed_words_outside_block': [hex(a) for a in other[:64]],
            'changed_words_outside_block_count': len(other),
            'rule': 'post-0x131e7c state = the 0x131e74 snapshot with manager+0x34 = 1 (derived-exact: the store is the '
                    'third instruction of a call-free block; the next block start shows it and only the documented '
                    'block stores, the RNG word, GS counters and resource state unchanged)'}

def analyse(evidence, out, rng, newgame_analysis, regress_newgame=None):
    runs = {r: os.path.join(evidence, 'runs', r) for r in RUNS}
    for r, p in runs.items():
        need(os.path.isdir(p), f'missing run {p}')
    os.makedirs(out)
    det = retire.determinism(runs, RETIRE_CPS + NEWGAME_CPS + RAW_CPS)
    need(all(v['identical'] for k, v in det.items() if isinstance(v, dict) and 'identical' in v),
         'runs are not deterministic: ' + json.dumps({k: v.get('identical') for k, v in det.items() if isinstance(v, dict)}))
    for cp in ('ui-publish-pre', 'ui-publish-post-block'):
        hs = {r: fsha(os.path.join(p, 'checkpoints', cp, 'ee-ram-32mb.bin')) for r, p in runs.items()}
        det[f'checkpoints/{cp}/ee-ram-32mb.bin'] = {'identical': len(set(hs.values())) == 1, 'sha256': hs}
        need(len(set(hs.values())) == 1, f'{cp} EE RAM differs between runs')
    a = runs['fill00-a']
    ev = read_jsonl(os.path.join(a, 'events.jsonl'))
    meta = json.load(open(os.path.join(a, 'run-meta.json')))
    need(meta.get('field_prefix') is True and meta['pad_step'] == 23 and meta['missing_checkpoints'] == []
         and meta['field_worker_entries'] == 1 and meta['field_stopped'] is True, 'run-meta does not describe a completed Field prefix')
    need(ev[-1]['ev'] == 'stop' and ev[-1]['kind'] == 'ui_prefix_published', 'capture does not end at the bracketed 0x131e7c stop')
    need(meta['checkpoints'] == list(RETIRE_CPS) + list(NEWGAME_CPS) + list(RAW_CPS), f'checkpoint order {meta["checkpoints"]}')

    tl, detail, counts = newgame.timeline(ev)
    for r in RUNS[1:]:
        need(newgame.timeline(read_jsonl(os.path.join(runs[r], 'events.jsonl')))[0] == tl, f'timeline differs in {r}')
    # Merge the Field rows by oracle index (the Newgame timeline has none of them).
    frows = field_rows(ev)
    by_i = {e['i']: e for e in ev}
    merged = []
    k = 0
    for i, row in frows:
        while k < len(detail) and detail[k]['oracle_i'] <= i:
            merged.append((detail[k], tl[k]))
            k += 1
        r = {x: y for x, y in row.items() if not x.startswith('_')}
        merged.append(({**r, 'oracle_i': i, 'vb': by_i[i].get('vb'), 'fn': by_i[i].get('fn'), 'parent': by_i[i].get('parent'),
                        'state_at': row.get('_state_at')}, r))
    merged += list(zip(detail[k:], tl[k:]))
    detail = [d for d, _ in merged]
    tl = [r for _, r in merged]
    need(all(x['oracle_i'] <= y['oracle_i'] for x, y in zip(detail, detail[1:])), 'merged timeline not in source order')

    # Boundaries: the Newgame ones (only Newgame checkpoint events considered) and the Field ones.
    newgame_ev = [e for e in ev if not (e['ev'] == 'checkpoint' and e['name'] in RAW_CPS)]
    cps = newgame.checkpoint_boundaries(newgame_ev)
    need(tuple(cps) == RETIRE_CPS + NEWGAME_CPS, f'Newgame checkpoints {list(cps)}')
    cpe = {e['name']: e for e in ev if e['ev'] == 'checkpoint'}
    for name in CHECKPOINTS[:-1]:
        e = cpe[name]
        prev = by_i[e['i'] - 1]
        if name == 'worker-entry':
            need(prev['ev'] == 'field_worker' and by_i[prev['i'] - 1]['ev'] == 'enter'
                 and by_i[prev['i'] - 1]['name'] == 'field_worker', 'worker-entry is not at the 0x132688 entry')
            cps[name] = (e, prev['i'] - 1)
        else:
            fn, ra = RETURN_TRIGGER[name]
            need(prev['ev'] == 'exit' and prev['name'] == fn, f'checkpoint {name} is not directly after its {fn} return')
            ent = by_i[prev['enter']]
            need(ra is None or ent['ra'] == ra, f'{name}: {fn} returned to {ent["ra"]}, not {ra}')
            cps[name] = (e, prev['i'] + 1)
    pre_e, post_e = cpe['ui-publish-pre'], cpe['ui-publish-post-block']
    pre_x = by_i[pre_e['i'] - 1]
    post_x = by_i[post_e['i'] - 1]
    need(pre_x['ev'] == 'ui_publish_pre' and post_x['ev'] == 'ui_publish_post_block', 'publication bracket events')
    proof = publish_proof(os.path.join(a, 'checkpoints', 'ui-publish-pre'), os.path.join(a, 'checkpoints', 'ui-publish-post-block'),
                          pre_x, post_x)
    # Nothing observed between the two block starts except the bracket events.
    between = [e for e in ev if pre_e['i'] < e['i'] < post_x['i']]
    need(all(e['ev'] in ('checkpoint', 'glyph_state') for e in between), f'events between the bracket stops: {[e["ev"] for e in between]}')
    need(pre_e['gs'] == post_e['gs'] and pre_e['rand_state_214ec8_58'] == post_e['rand_state_214ec8_58'],
         'GS counters or the RNG word changed across the bracket')
    pre_s = read_jsonl(os.path.join(a, 'checkpoints', 'ui-publish-pre', 'state.jsonl'))[0]
    post_s = read_jsonl(os.path.join(a, 'checkpoints', 'ui-publish-post-block', 'state.jsonl'))[0]
    req = retire.resource_equal(pre_s, post_s)
    need(all(req.values()), f'resource state changed across the bracket: {[k for k, v in req.items() if not v]}')
    need(fsha(os.path.join(a, 'checkpoints', 'ui-publish-pre', 'vram.bin'))
         == fsha(os.path.join(a, 'checkpoints', 'ui-publish-post-block', 'vram.bin')), 'VRAM changed across the bracket')
    fp, fq = dict(pre_s['field']), dict(post_s['field'])
    need(fp.pop('w34') == 0 and fq.pop('w34') == 1 and fp.pop('manager_440_sha256') and fq.pop('manager_440_sha256')
         and fp == fq, 'field words other than +0x34 changed across the bracket')
    proof['resource_state_equal'] = True
    proof['vram_equal'] = True
    # The derived checkpoint: boundary just before the post-block event; its rows end with the 0x131e7c store.
    cps['ui-prefix-published'] = (pre_e, post_x['i'])
    pre_ram = open(os.path.join(a, 'checkpoints', 'ui-publish-pre', 'ee-ram-32mb.bin'), 'rb').read()
    proof['tex0_register_332438'] = word(pre_ram, 0x332438)
    need(proof['tex0_register_332438'] == 6, '0x183ff0 did not store the TEX0_1 register id 6 at 0x332438')

    successor_i = cps['retire-successor-entry'][1]
    rng_rows = read_jsonl(rng)
    draws, draw_detail = newgame.rng_draws(ev, rng_rows, successor_i)
    window = [d for d in draw_detail if d['oracle_i'] > cps['field-constructor-entry'][1]]
    ptrs = restore.table_ptrs(ev)
    need(len(ptrs) == 1, 'expected one selector-1 title construction')
    trav_of, trav = {}, 0
    for e in ev:
        if e['ev'] == 'top' and e['pc'] == newgame.SCHEDULER_PC:
            trav += 1
        trav_of[e['i']] = trav

    write_x(os.path.join(out, 'events-detail.jsonl'), jl(detail))
    neutral = {}
    for name in NEWGAME_CPS:
        e, boundary = cps[name]
        fs = read_jsonl(os.path.join(a, 'checkpoints', name, 'state.jsonl'))[0]
        pre = [r for r in restore.prefix(detail, tl, boundary)]
        need(not any(r['kind'] == 'field' for r in pre), f'Field rows before {name}')
        st = restore.native_state(fs, name, ptrs)
        st['coverage'] = {'lifecycle': 'newgame', 'checkpoint': name, 'memory': MEMORY}
        gen = {'events.jsonl': jl(pre), 'state.json': json.dumps(st, separators=(',', ':')) + '\n',
               'rng.jsonl': jl(draws[:sum(1 for d in draw_detail if d['oracle_i'] < boundary)]),
               'session.json': json.dumps(newgame.session_state(fs), separators=(',', ':')) + '\n',
               'effects.json': json.dumps({'effect106': retire.effect_state(fs['title_effect']) if fs['title_effect_slot'] >= 0
                                           else None}, separators=(',', ':')) + '\n'}
        res = {}
        for f, text in gen.items():
            res[f] = hashlib.sha256(text.encode()).hexdigest() == fsha(os.path.join(newgame_analysis, name, f))
        neutral[name] = res
    neutral_ok = all(all(v.values()) for v in neutral.values())
    need(neutral_ok, 'the Field capture does not reproduce the pinned Newgame checkpoints: ' +
         json.dumps({k: [f for f, ok in v.items() if not ok] for k, v in neutral.items() if not all(v.values())}))
    if regress_newgame:
        rev = read_jsonl(os.path.join(regress_newgame, 'events.jsonl'))
        stop_i = cps['field-constructor-entry'][0]['i']
        mine = [e for e in ev if e['i'] <= stop_i]
        theirs = [e for e in rev if e['i'] <= stop_i]
        need(rev[-1]['ev'] == 'stop' and rev[-1]['kind'] == 'field_constructor_entry', 'regression run is not a Newgame capture')
        need(mine == theirs, 'the Newgame-mode run of this oracle differs from the Field run before the Newgame stop')
        neutral['regress_newgame_events_equal_to_stop'] = True

    cp_info = {}
    for name in CHECKPOINTS:
        e, boundary = cps[name]
        raw = 'ui-publish-pre' if name == 'ui-prefix-published' else name
        fs = read_jsonl(os.path.join(a, 'checkpoints', raw, 'state.jsonl'))[0]
        pre = restore.prefix(detail, tl, boundary)
        if name == 'ui-prefix-published':
            pre = pre + [{'kind': 'field', 'op': 'ui-available'}]
            fs = dict(fs)
            fs['field'] = dict(fs['field'], w34=1)
        # The env-bind row carries the 183ff0 outputs of the state right after that return (texenv-bound).
        for r, d in zip(pre, detail):
            if r.get('kind') == 'field' and r['op'] == 'env-bind':
                bound = read_jsonl(os.path.join(a, 'checkpoints', 'texenv-bound', 'state.jsonl'))[0]['field']
                r['tex0'] = tex0_hex(bound)
                r['region'] = [int(x, 16) for x in bound['env_332450']]
        resource = [x for x in pre if x['kind'] != 'phase']
        d = os.path.join(out, name)
        os.makedirs(d)
        write_x(os.path.join(d, 'events.jsonl'), jl(pre))
        st = restore.native_state(class_free_state(fs), raw, ptrs)
        st['coverage'] = {'lifecycle': LIFECYCLE, 'checkpoint': name, 'memory': MEMORY}
        write_x(os.path.join(d, 'state.json'), json.dumps(st, separators=(',', ':')) + '\n')
        fx = {'effect106': retire.effect_state(fs['title_effect']) if fs['title_effect_slot'] >= 0 else None}
        write_x(os.path.join(d, 'effects.json'), json.dumps(fx, separators=(',', ':')) + '\n')
        nd = sum(1 for x in draw_detail if x['oracle_i'] < boundary)
        write_x(os.path.join(d, 'rng.jsonl'), jl(draws[:nd]))
        ss = newgame.session_state(fs)
        write_x(os.path.join(d, 'session.json'), json.dumps(ss, separators=(',', ':')) + '\n')
        fj = field_json(fs['field'], pre)
        write_x(os.path.join(d, 'field.json'), json.dumps(fj, separators=(',', ':')) + '\n')
        cpd = {r: os.path.join(runs[r], 'checkpoints', raw) for r in RUNS}
        vram = retire.vram_analysis(os.path.join(cpd['fill00-a'], 'vram.bin'), os.path.join(cpd['fillff-a'], 'vram.bin'),
                                 os.path.join(cpd['fill00-a'], 'vram-last-writer.bin'),
                                 os.path.join(cpd['fill00-a'], 'vram-upload-ever.bin'), d)
        cp_info[name] = {'oracle_i': e['i'], 'boundary_oracle_i': boundary, 'vb': e['vb'], 'pc': e['pc'],
                         'traversal': trav_of[e['i']], 'resource_events': len(resource),
                         'events_by_kind': dict(sorted(collections.Counter(
                             x['kind'] if x['kind'] not in ('effect', 'mode', 'ui', 'glyph', 'field') else x['kind'] + ':' + x['op']
                             for x in resource).items())),
                         'rng_draws_before': nd, 'gs': e['gs'], 'stack_depth': e['stack_depth'],
                         'rand_state': e['rand_state_214ec8_58'], 'field': fj, 'counter_38': fs['field']['w38'],
                         'reserve_class_3abb14': fs['field']['reserve_3abb14'], 'vram': vram,
                         'derived': name == 'ui-prefix-published'}
    need(all(cp_info[x]['boundary_oracle_i'] <= cp_info[y]['boundary_oracle_i'] for x, y in zip(CHECKPOINTS, CHECKPOINTS[1:])),
         'checkpoint order')
    fce = cps['field-constructor-entry'][0]
    sched = {'field_constructor_entry': {'vb': fce['vb'], 'traversal': trav_of[fce['i']]},
             'checkpoints': {n: {'vb': cp_info[n]['vb'], 'traversal': cp_info[n]['traversal'], 'counter_38': cp_info[n]['counter_38']}
                             for n in CHECKPOINTS},
             'note': 'vblanks/traversals/+0x38 counts are scheduling (observer- and timing-dependent); never compared'}
    ref = {'schema': 'okage.field-prefix-original-reference.v1', 'route': 'newgame-field-prefix',
           'checkpoint_rule': 'worker-entry precedes every event of 0x132688; the other source checkpoints follow every '
                              'event of their call; ui-prefix-published is the 0x131e74 snapshot plus the 0x131e7c store',
           'checkpoints': cp_info, 'timeline_counts': dict(sorted(counts.items())), 'scheduling': sched,
           'rng': {'provenance': 'every draw is a hooked rand() observation (LCG chain checked); the Field window has '
                                 f'{len(window)} draws', 'window_draws': len(window), 'total': len(draws)},
           'publish_proof': proof, 'determinism': det, 'newgame_neutrality': neutral,
           'unmodeled_source_effects': list(UNMODELED)}
    need(len(window) == 0, f'{len(window)} RNG draws in the Field window (the native model makes none)')
    ref['control_minimal'] = control(os.path.join(evidence, 'runs', CONTROL), a)
    ref['problems'] = [] if ref['control_minimal']['failures'] == [] else ['control:' + f for f in ref['control_minimal']['failures']]
    return ref

def control(ctl, a):
    """The minimal-hook control (Newgame minimal hooks plus 0x132688 and the bracket) at its shared Field checkpoints."""
    need(os.path.isdir(ctl), 'the minimal-hook control run is required')
    cmeta = json.load(open(os.path.join(ctl, 'run-meta.json')))
    need(cmeta.get('field_prefix') is True and cmeta['minimal'] is True, 'control run-meta')
    out = {'checkpoints': cmeta['checkpoints'], 'missing': cmeta['missing_checkpoints'], 'vblanks': cmeta['vblanks'],
           'stop_pc': cmeta['field_stop_pc']}
    cmp = {}
    keys = tuple(k for k in retire.RESOURCE_STATE_KEYS if k not in ('allocations', 'allocation_serials'))
    for name in CONTROL_CPS:
        cs = read_jsonl(os.path.join(ctl, 'checkpoints', name, 'state.jsonl'))[0]
        hs = read_jsonl(os.path.join(a, 'checkpoints', name, 'state.jsonl'))[0]
        cf, hf = dict(cs['field']), dict(hs['field'])
        row = {'resource_state_equal': retire.resource_equal(cs, hs, keys),
               'session_state_equal': newgame.session_state(cs) == newgame.session_state(hs),
               'field_equal_except_counter': {k: cf[k] == hf[k] for k in cf if k != 'w38'},
               'counter_38': [cf['w38'], hf['w38']],
               'vram_fill00_equal': fsha(os.path.join(ctl, 'checkpoints', name, 'vram.bin'))
               == fsha(os.path.join(a, 'checkpoints', name, 'vram.bin'))}
        cmp[name] = row
    for name in ('ui-publish-pre', 'ui-publish-post-block'):
        cmp[name]['ee_ram_equal'] = fsha(os.path.join(ctl, 'checkpoints', name, 'ee-ram-32mb.bin')) \
            == fsha(os.path.join(a, 'checkpoints', name, 'ee-ram-32mb.bin'))
    out['checkpoints_compared'] = cmp
    out['failures'] = sorted(f'{n}:{k}' for n, c in cmp.items() for k, v in c.items()
                             if v is False or (isinstance(v, dict) and not all(v.values())))
    out['note'] = ('the control has no rand hook and no ledgers; resource/session/field/VRAM (fill 00) state and, at the '
                   'bracket, all of EE RAM are compared; the +0x38 counter is scheduling and only reported')
    return out

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('evidence')
    p.add_argument('--out', required=True)
    p.add_argument('--rng', required=True)
    p.add_argument('--newgame-analysis', required=True)
    p.add_argument('--regress-newgame')
    a = p.parse_args(argv)
    try:
        ref = analyse(a.evidence, a.out, a.rng, a.newgame_analysis, a.regress_newgame)
    except Fail as exc:
        print('FAIL: ' + str(exc), file=sys.stderr)
        return 1
    write_x(os.path.join(a.out, 'reference.json'), json.dumps(ref, indent=1) + '\n')
    for name, info in ref['checkpoints'].items():
        print(f'  {name:26s} vb {info["vb"]:5d} trav {info["traversal"]:5d} events {info["resource_events"]:5d} '
              f'draws {info["rng_draws_before"]} handles {info["field"]["manager_60"]}/{info["field"]["resource_48"]}')
    print('problems:', ref['problems'])
    return 0 if not ref['problems'] else 1

if __name__ == '__main__':
    sys.exit(main())
