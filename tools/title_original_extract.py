#!/usr/bin/env python3
"""Extract the Title logo/title original reference from `okage-startup-oracle --title` captures.

usage: title_original_extract.py RUNS_DIR --out ANALYSIS_DIR [--startup STARTUP_RUN] [--rng RNG_RUN]

RUNS_DIR holds fill00-a, fill00-b (VRAM pre-filled 0x00) and fillff-a, fillff-b (0xff), all from
`okage-startup-oracle DISC ELF OUT FILL --title`, and optionally control-minimal-fill00 (`--title --minimal`).
--startup: the Startup fill00-a run (final-state.jsonl + events.jsonl) and --rng: the RNG rng-a run, for the
prefix cross-checks (optional; recorded in reference.json).

Writes into ANALYSIS_DIR (must not exist):
  events.jsonl                 ordered resource timeline in the NATIVE timeline schema (the title original-reference contract)
  events-detail.jsonl          the same events with source attribution (oracle event index, vblank, caller, top step)
  final-state.json             final allocator/handle/record/image-list/material state in the NATIVE state.json schema
  vram-written-mask.bin        per byte bit0/bit1 = low/high nibble LAST written by a host-to-local transfer
                               (paired-fill derived, and not touched by a later draw/local-to-local writer per the GS ledger)
  vram-written-values.bin      the value of those nibbles (other nibbles 0)
  vram-invalidated-mask.bin    nibbles whose last writer is a draw or a local-to-local transfer (native must be UNKNOWN)
  gs-writers.json              classified GS writers (host-to-local, draw classes with FRAME/ZBUF/TEX0, local-to-local, CLUT loads)
  reference.json               determinism, counts, lifecycle/storage/cache/queue context, traversal, cross-checks
Reads only; exits non-zero if the runs disagree or a cross-check fails.
"""
import argparse
import collections
import hashlib
import json
import os
import sys

VRAM = 1 << 22
RUNS = ('fill00-a', 'fill00-b', 'fillff-a', 'fillff-b')
MODEL_LABEL_RULE = 'upper-case archive stem of the archive mounted at the 0x186c48 call + "/" + its path argument'

def sha(b):
    return hashlib.sha256(b).hexdigest()

def fsha(path):
    with open(path, 'rb') as f:
        return sha(f.read())

def read_jsonl(path):
    with open(path) as f:
        return [json.loads(l) for l in f if l.strip()]

def jl(rows):
    return ''.join(json.dumps(r, separators=(',', ':')) + '\n' for r in rows)

class Fail(Exception):
    pass

def need(cond, msg):
    if not cond:
        raise Fail(msg)

def timeline(events):
    enters = {}
    out, detail = [], []
    live = set()
    archive = None
    xmd_path = None
    counts = collections.Counter()

    def add(row, e, ent=None):
        out.append(row)
        d = dict(row)
        src = ent or e
        d.update({'oracle_i': e['i'], 'vb': e.get('vb'), 'top': e.get('top'), 'fn': src.get('fn'),
                  'caller_ra': src.get('ra'), 'parent': src.get('parent')})
        detail.append(d)
        counts[row['kind']] += 1

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
                h = int(e['handle_or_record'])
                if e.get('record', -1) >= 0:
                    add({'kind': 'retire', 'handle': h}, e)
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
                # 0x101058 runs when each mode-8 child is retired (after selector 3 and after selector 0)
                add({'kind': 'phase', 'name': 'mode8-child-resource-retire'}, e)
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
        elif ev == 'prefix_end':
            add({'kind': 'phase', 'name': 'prefix-end-0x100618'}, e)
        elif ev == 'stop':
            add({'kind': 'phase', 'name': 'stop-title-constructor-return'}, e)
    return out, detail, counts

# ----------------------------------------------------------------------------------------------
def native_state(fs, tl_events, model_table_ptr):
    """Final state in the native state.json schema (see the title original-reference contract)."""
    serial = {i: s for i, s in fs['allocation_serials']}
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
    tables = [t for t in fs['material_tables'] if t and int(t['ptr'], 16) == model_table_ptr]
    need(len(tables) == 1, 'NIGHT_V material table not found exactly once in the final material list')
    need(fs['reserve_class'] == 0 and fs['reserved1'] == [],
         'class-1 reserved queue in use at the stop: native schema carries only the class-0 queue')
    return {
        'coverage': {'lifecycle': 'title-constructor-return', 'memory': 'host-uploads-known-draw-writes-unknown'},
        'allocations': live_allocs, 'handles': handles, 'records': records,
        'hash': fs['hash'], 'reserved': fs['reserved'], 'free_records': fs['free_records'],
        'free_extras': fs['free_extras'], 'available': fs['vram']['available'], 'cursors': fs['vram']['cursors'],
        'free_queue_ids': fs['free_queue_ids'], 'occupancy_hex': fs['occupancy_hex'],
        'image_list': fs['image_list'], 'materials': [[i, h] for i, h in enumerate(tables[0]['handles'])],
    }

# ----------------------------------------------------------------------------------------------
def nibbles(mask_bytes):
    return sum(((m & 1) + ((m >> 1) & 1)) for m in mask_bytes)

def vram_analysis(runs, out):
    v00 = open(os.path.join(runs['fill00-a'], 'vram.bin'), 'rb').read()
    vff = open(os.path.join(runs['fillff-a'], 'vram.bin'), 'rb').read()
    writer = open(os.path.join(runs['fill00-a'], 'vram-last-writer.bin'), 'rb').read()
    ever = open(os.path.join(runs['fill00-a'], 'vram-upload-ever.bin'), 'rb').read()
    need(len(v00) == len(vff) == len(writer) == len(ever) == VRAM, 'VRAM images must be 4 MiB')
    U = bytearray(VRAM)
    T = bytearray(VRAM)
    vals = bytearray(VRAM)
    st = collections.Counter()
    first_mismatch = None
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
                if first_mismatch is None:
                    first_mismatch = (i, bit, a, b, w, e)
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
         f'paired-fill written mask differs from the ledger upload footprint ({st["paired_vs_ledger_upload_mismatch"]} nibbles, '
         f'first {first_mismatch})')
    need(st['unknown_writer_code'] == 0 and st['sub_nibble_anomalies'] == 0, f'VRAM anomalies: {dict(st)}')
    for name, data in (('vram-written-mask.bin', U), ('vram-written-values.bin', vals), ('vram-invalidated-mask.bin', T)):
        with open(os.path.join(out, name), 'xb') as f:
            f.write(bytes(data))
    return {'known_nibbles_required': nibbles(U), 'unknown_nibbles_required': nibbles(T),
            'ever_uploaded_nibbles': nibbles(ever), 'stats': dict(st),
            'mask_sha256': sha(bytes(U)), 'values_sha256': sha(bytes(vals)), 'invalidated_sha256': sha(bytes(T))}

def gs_writers(ledger, vram_info):
    tr = [r for r in ledger if r['kind'] == 'gs_transfer']
    draws = [r for r in ledger if r['kind'] == 'gs_draw_class']
    l2l = [r for r in ledger if r['kind'] == 'gs_local_to_local']
    clut = [r for r in ledger if r['kind'] == 'gs_clut_class']
    summ = [r for r in ledger if r['kind'] == 'gs_title_summary']
    gsum = [r for r in ledger if r['kind'] == 'gs_summary']
    need(len(summ) == 1 and len(gsum) == 1, 'ledger summaries missing (not a --title capture?)')
    by_dpsm = collections.Counter(r['dpsm'] for r in tr)
    return {
        'rules': {
            'host_to_local': 'every nibble written by Play\'s transfer handler for DPSM (same pixel stepping; PSMCT24 writes '
                             'the low 24 bits, T8H/T4HL/T4HH their lane); writer code 1',
            'draw': 'every primitive assembled from PRIM/XYZ2/XYZF2/XYZ3/XYZF3 (vertex queue, XYZ3/XYZF3 without kick): the '
                    'inclusive pixel rectangle [floor(min),ceil(max)] of its vertices in window coordinates (12.4 minus XYOFFSET) '
                    'clipped to SCISSOR, in the FRAME buffer (FBP/FBW/PSM, nibbles not fully masked by FBMSK; 24-bit formats low '
                    '6 nibbles; 16-bit formats all 4 nibbles unless FBMSK is all ones) and in the Z buffer (ZBP/ZPSM, FBW) unless '
                    'ZMSK. Not written only when TEST ZTE=1/ZTST=NEVER, or ATE=1/ATST=NEVER/AFAIL=KEEP (AFAIL FB_ONLY/ZB_ONLY/RGB_ONLY '
                    'drop Z/frame/Z+alpha). Context from PRMODECONT/PRMODE. Conservative superset of real coverage; writer code 2',
            'local_to_local': 'destination rectangle (DBP/DBW/DPSM, DSAX/DSAY, RRW x RRH) of TRXDIR=2; NOT executed by the null '
                              'renderer; writer code 3',
            'clut_load': 'CLUT loads read local memory into the CLUT buffer; no local-memory write',
            'last_writer': 'per nibble, in GS processing order; a later host-to-local write makes a drawn nibble known again',
        },
        'host_to_local': {'count': len(tr), 'complete': sum(1 for r in tr if r['complete']),
                          'incomplete': [{k: r[k] for k in ('seq', 'dbp', 'dbw', 'dpsm', 'rrw', 'rrh', 'expected_bytes',
                                                             'received_bytes')} for r in tr if not r['complete']],
                          'bytes_received': sum(r['received_bytes'] for r in tr), 'by_dpsm': {str(k): v for k, v in sorted(by_dpsm.items())},
                          'ct24_chunks_not_multiple_of_3': sum(r.get('ct24_chunks_not_multiple_of_3', 0) for r in tr)},
        'draw_classes': draws, 'local_to_local': l2l, 'clut_classes': clut,
        'summary': summ[0], 'gs_summary': gsum[0], 'vram': vram_info,
    }

# ----------------------------------------------------------------------------------------------
def strip_fill(fs):
    d = json.loads(json.dumps(fs))
    d['vram'].pop('run_fill')
    d['vram'].pop('run_vram_sha256')
    return d

EE_CROSS_REFERENCE = ('ee_event', 'first_ee_event', 'last_ee_event')

def ledger_without_ee_cross_reference(path):
    """GS ledger rows minus the approximate EE event cross-reference (read on the GS thread from a counter
    the EE observer thread advances: host-timing dependent by construction, informational only)."""
    rows = read_jsonl(path)
    for r in rows:
        for k in EE_CROSS_REFERENCE:
            r.pop(k, None)
    return rows

def determinism(runs):
    res = {}
    for name in ('events.jsonl', 'disc-opens.jsonl', 'vram-last-writer.bin', 'vram-upload-ever.bin',
                 'ee-ram-32mb.bin', 'ee-ram-after-10bc78.bin'):
        hs = {r: fsha(os.path.join(p, name)) for r, p in runs.items()}
        res[name] = {'identical': len(set(hs.values())) == 1, 'sha256': hs}
    hs = {r: fsha(os.path.join(p, 'gs-ledger.jsonl')) for r, p in runs.items()}
    led = {r: ledger_without_ee_cross_reference(os.path.join(p, 'gs-ledger.jsonl')) for r, p in runs.items()}
    res['gs-ledger.jsonl (ee_event cross-reference removed)'] = {
        'identical': all(led[r] == led['fill00-a'] for r in led), 'raw_sha256': hs,
        'raw_identical': len(set(hs.values())) == 1}
    fss = {r: strip_fill(read_jsonl(os.path.join(p, 'final-state.jsonl'))[0]) for r, p in runs.items()}
    res['final-state.jsonl (fill fields removed)'] = {'identical': all(fss[r] == fss['fill00-a'] for r in fss)}
    pss = {}
    for r, p in runs.items():
        s = read_jsonl(os.path.join(p, 'state-at-100618.jsonl'))[0]
        s['vram'].pop('run_fill')
        pss[r] = s
    res['state-at-100618.jsonl (fill field removed)'] = {'identical': all(pss[r] == pss['fill00-a'] for r in pss)}
    vr = {r: fsha(os.path.join(p, 'vram.bin')) for r, p in runs.items()}
    res['vram.bin within fill'] = {'identical': vr['fill00-a'] == vr['fill00-b'] and vr['fillff-a'] == vr['fillff-b'], 'sha256': vr}
    metas = {r: json.load(open(os.path.join(p, 'run-meta.json'))) for r, p in runs.items()}
    res['run-meta'] = {r: {k: m[k] for k in ('events', 'breakpoint_stops', 'vblanks', 'wall_seconds', 'rand_calls', 'stop_pc')}
                       for r, m in metas.items()}
    return res

def startup_cross_check(prefix_state, startup_run):
    t = read_jsonl(os.path.join(startup_run, 'final-state.jsonl'))[0]
    out = {}
    for k in ('handles', 'records', 'hash', 'live_handles', 'reserved', 'free_records', 'free_extras', 'descriptors'):
        out[k] = prefix_state[k] == t[k]
    out['allocations'] = [dict(a) for a in prefix_state['allocations']] == t['allocations']
    for k in ('available', 'cursors', 'occupancy_sha256', 'nodes_sha256'):
        out['vram.' + k] = prefix_state['vram'][k] == t['vram'][k]
    out['rand_state'] = prefix_state['rand_state_214ec8_58'] == t['rand_state_214ec8_58']
    return out

def upload_rows(events, until_prefix):
    rows = []
    for e in events:
        if e['ev'] == 'prefix_end' and until_prefix:
            break
        if e['ev'] == 'enter' and e['name'] == 'upload':
            rows.append((e['desc']['raw'], e['x'], e['y'], e['w'], e['h'], e.get('src_sha256')))
    return rows

def rand_rows(events, until_prefix):
    rows = []
    for e in events:
        if e['ev'] == 'prefix_end' and until_prefix:
            break
        if e['ev'] == 'rand':
            rows.append((e['ra'], e['state']))
    return rows

# ----------------------------------------------------------------------------------------------
def analyse(args):
    runs = {r: os.path.join(args.runs, r) for r in RUNS}
    for r, p in runs.items():
        need(os.path.isdir(p), f'missing run {p}')
    os.makedirs(args.out)
    det = determinism(runs)
    need(all(v['identical'] for k, v in det.items() if isinstance(v, dict) and 'identical' in v and k != 'ee-ram-32mb.bin'),
         'runs are not deterministic: ' + json.dumps({k: v.get('identical') for k, v in det.items() if isinstance(v, dict)}))

    ev = read_jsonl(os.path.join(runs['fill00-a'], 'events.jsonl'))
    tl, detail, counts = timeline(ev)
    for r in RUNS[1:]:
        need(timeline(read_jsonl(os.path.join(runs[r], 'events.jsonl')))[0] == tl, f'timeline differs in {r}')
    fs = read_jsonl(os.path.join(runs['fill00-a'], 'final-state.jsonl'))[0]
    # The NIGHT_V table is the 0x182e28 call inside the selector-1 constructor (startup models before
    # 0x100618 build texture-less tables; they produce no material events).
    s1 = [e for e in ev if e['ev'] == 'enter' and e['name'] == 'mode8_construct' and e['selector'] == 1]
    need(len(s1) == 1, 'expected exactly one 0x12c350(selector 1)')
    s1_exit = [e for e in ev if e['ev'] == 'exit' and e['name'] == 'mode8_construct' and e['enter'] == s1[0]['i']]
    need(len(s1_exit) == 1 and s1_exit[0]['i'] == ev[-2]['i'], 'selector-1 return is not the last event before the stop')
    need(ev[-1]['ev'] == 'stop' and ev[-1]['kind'] == 'title_constructor_return', 'capture does not end at the title stop')
    mt = [e for e in ev if e['ev'] == 'exit' and e['name'] == 'material_table' and s1[0]['i'] < e['i'] < s1_exit[0]['i']]
    need(len(mt) == 1, f'expected one 0x182e28 material table inside 0x12c350(selector 1), saw {len(mt)}')
    need(not any(e['ev'] == 'enter' and e['name'] in ('title_update', 'cached_model_loader') for e in ev),
         '0x1296f8 or 0x1bba88 reached before the stop')
    state = native_state(fs, tl, int(mt[0]['table']['ptr'], 16))
    need(state['materials'] == [[i, h] for i, h in enumerate(mt[0]['table']['handles'])],
         'material table at the stop differs from the table at 0x182e28 return')

    with open(os.path.join(args.out, 'events.jsonl'), 'x') as f:
        f.write(jl(tl))
    with open(os.path.join(args.out, 'events-detail.jsonl'), 'x') as f:
        f.write(jl(detail))
    with open(os.path.join(args.out, 'final-state.json'), 'x') as f:
        json.dump(state, f, separators=(',', ':'))
        f.write('\n')

    vinfo = vram_analysis(runs, args.out)
    ledger = read_jsonl(os.path.join(runs['fill00-a'], 'gs-ledger.jsonl'))
    gw = gs_writers(ledger, vinfo)
    need(gw['summary']['unsupported_targets'] == 0, 'GS writers with unsupported formats: the VRAM claim would be incomplete')
    need(gw['gs_summary']['resets'] == 0, 'GS reset during the run')
    with open(os.path.join(args.out, 'gs-writers.json'), 'x') as f:
        json.dump(gw, f, indent=1)
        f.write('\n')

    # ---- reference: lifecycle context ----------------------------------------------------
    def rows(name, which='enter'):
        return [e for e in ev if e['ev'] == which and e['name'] == name]
    by_i = {e['i']: e for e in ev}
    exits = {e['enter']: e for e in ev if e['ev'] == 'exit'}
    m8 = [{'selector': e['selector'], 'enter_i': e['i'], 'enter_vb': e['vb'], 'return_vb': exits[e['i']]['vb'] if e['i'] in exits else None,
           'image_list_before': e['image_list'], 'image_list_after': exits[e['i']]['image_list'] if e['i'] in exits else None,
           'reserve_class': e['reserve_class'], 'cache_switch': e['cache_switch'], 'storage_words': e['storage_words']}
          for e in rows('mode8_construct')]
    ticks = [{'tick': e['tick'], 'vb': e['vb'], 'i': e['i']} for e in rows('logo_tick') if e['tick'] in (1, 180, 270, 480, 600)]
    reqs = [{k: e[k] for k in ('i', 'vb', 'mode', 'selector', 'arg3', 'ra')} for e in rows('mode_request')]
    storage = [{'fn': e['name'], 'i': e['i'], 'vb': e['vb'], 'result': e.get('result'), 'param_words': e.get('param_words'),
                'storage_words': e.get('storage_words')} for e in ev if e['ev'] == 'exit' and e['name'] in ('storage_probe', 'storage_init')]
    xmd = [{'i': e['i'], 'vb': e['vb'], 'names': e['xmd_names'], 'cache_switch': e['cache_switch'], 'lower_switch': e['lower_switch'],
            'reserve_class': e['reserve_class'], 'reserved0': e['reserved0'], 'reserved1': e['reserved1']} for e in rows('xmd_load')]
    mtab = mt[0]
    succ = [{'i': e['i'], 'handle': e['handle'], 'record': e['record'], 'successor': exits[e['i']]['successor'],
             'parent': e['parent']} for e in rows('handle_successor')]
    reserves = [{'i': e['i'], 'count': e['count'], 'class': e['reserve_class'], 'reserved0_before': e['reserved0'],
                 'first': exits[e['i']]['first'], 'reserved0_after': exits[e['i']]['reserved0'], 'parent': e['parent']}
                for e in rows('handle_reserve')]
    lookups = [{'i': e['i'], 'name': e['str'], 'handle': exits[e['i']]['handle']} for e in rows('handle_lookup')]
    cache_sets = [{'fn': e['name'], 'i': e['i'], 'vb': e['vb'], 'value': e['value']} for e in ev if e['ev'] == 'enter'
                  and e['name'] in ('cache_enable_set', 'lower_enable_set')]
    cls_sets = [e for e in ev if e['ev'] == 'enter' and e['name'] == 'reserve_class_set']
    handle_ops = [e for e in ev if e['ev'] == 'enter' and e['name'] in ('handle_create', 'handle_alias', 'handle_reserve', 'handle_lookup')]
    class_windows = {
        'writes': len(cls_sets), 'writes_of_1': sum(1 for e in cls_sets if e['value'] == 1),
        'callers_ra': sorted(set(e['ra'] for e in cls_sets)), 'top_steps': sorted(set(e['top'] for e in cls_sets)),
        'handle_operations': len(handle_ops),
        'handle_operations_with_class_1': sum(1 for e in handle_ops if e.get('reserve_class') == 1),
        'first_writes': [{k: e[k] for k in ('i', 'vb', 'value', 'ra', 'top')} for e in cls_sets[:6]],
    }
    file_reads = [{'i': e['i'], 'file': e['file'], 'bytes': e['bytes_read'], 'sha256': e['sha256']} for e in ev
                  if e['ev'] == 'exit' and e['name'] == 'file_read_raw']
    prefix_i = [e['i'] for e in ev if e['ev'] == 'prefix_end']
    need(len(prefix_i) == 1, 'prefix_end missing')
    post = [e for e in ev if e['i'] > prefix_i[0]]
    rands_post = [e for e in post if e['ev'] == 'rand']
    qwc = [e for e in ev if e['ev'] == 'exit' and e['name'] == 'upload_qwc' and 'upload_sha256' in e]
    gs_tr = [r for r in ledger if r['kind'] == 'gs_transfer']
    up_vs_gs = {'uploads': len(qwc), 'gs_transfers': len(gs_tr), 'mismatches': []}
    for k, (q, g) in enumerate(zip(qwc, gs_tr)):
        d = q['upload_desc']
        problems = []
        if g['dbp'] != d['tbp'] or g['dpsm'] != d['psm']:
            problems.append('target')
        if g['received_bytes'] != q['upload_bytes'] or g['image_sha256'] != q['upload_sha256']:
            problems.append('payload')
        if problems:
            up_vs_gs['mismatches'].append({'n': k, 'problems': problems, 'upload': q['upload_desc'], 'qwc': q['qwc'],
                                           'gs': {x: g[x] for x in ('dbp', 'dpsm', 'rrw', 'rrh', 'expected_bytes', 'received_bytes')}})
    need(len(qwc) == len(gs_tr) and not up_vs_gs['mismatches'], 'uploads and GS transfers disagree: ' + json.dumps(up_vs_gs)[:2000])
    incomplete = [{'n': k, 'upload_desc': q['upload_desc'], 'qwc': q['qwc'], 'gs_expected_bytes': g['expected_bytes'],
                   'gs_received_bytes': g['received_bytes']} for k, (q, g) in enumerate(zip(qwc, gs_tr)) if not g['complete']]

    ref = {
        'schema': 'okage.title-original-reference.v1',
        'timeline_counts': {k: v for k, v in sorted(counts.items())},
        'timeline_ordering_rule': 'entry: upload, retire, bind, name; return: alloc, ref, free, handle, reserve, material',
        'normalisations': {
            'descriptors': 'replaced by allocation id (bits 20..31); serial = 1-based count of successful 0x1cf008 calls since boot',
            'ref': '0x1cee50 calls on the null descriptor (no effect) omitted; refs = clamped count after the call',
            'free': 'only 0x1cf728 calls that freed (available count +1); calls on null/referenced descriptors omitted',
            'retire/bind/name': 'calls on a negative or unmapped handle (source early return, no effect) omitted',
            'bind': '0x1cd298 only (0x1cd598 tail-jumps into it); alloc -1 = null descriptor',
            'alloc': 'class/psm/w/h = 0x1cf008 arguments; tbp from the returned descriptor; failure -> alloc -1, tbp -1',
            'material.model': MODEL_LABEL_RULE,
            'phase': 'informational markers, not compared',
        },
        'mode8_constructions': m8, 'logo_ticks': ticks, 'mode_requests': reqs, 'storage_events': storage,
        'xmd_loads': xmd, 'material_table': {'i': mtab['i'], 'table': mtab['table'], 'enter': by_i[mtab['enter']]},
        'handle_successor_traversal': succ, 'reserves': reserves, 'lookups': lookups, 'switch_writes': cache_sets,
        'reserve_class_windows': class_windows,
        'file_reads': file_reads,
        'final_context': {k: fs[k] for k in ('reserve_class', 'reserved1', 'cache_switch', 'lower_switch', 'image_list_count',
                                             'record_free_order', 'extra_free_order', 'inuse_ring_ids', 'handle_range_sha256',
                                             'handle_nodes_sha256')},
        'rand_after_prefix': {'calls': len(rands_post), 'first': rands_post[:5]},
        'uploads_vs_gs_transfers': up_vs_gs, 'incomplete_transfers': incomplete,
        'determinism': det,
        'stop': [e for e in ev if e['ev'] == 'stop'],
        'prefix_end': by_i[prefix_i[0]],
    }
    ctl = os.path.join(args.runs, 'control-minimal-fill00')
    if os.path.isdir(ctl):
        cev = read_jsonl(os.path.join(ctl, 'events.jsonl'))
        cfs = read_jsonl(os.path.join(ctl, 'final-state.jsonl'))[0]
        keys = ('handles', 'records', 'hash', 'live_handles', 'reserved', 'free_records', 'free_extras', 'descriptors')
        ref['control_minimal'] = {
            'mode8_selectors': [e['selector'] for e in cev if e['ev'] == 'enter' and e['name'] == 'mode8_construct'],
            'vblanks': json.load(open(os.path.join(ctl, 'run-meta.json')))['vblanks'],
            'state_equal': {k: cfs[k] == fs[k] for k in keys},
            'vram_cursors_occupancy_equal': cfs['vram']['cursors'] == fs['vram']['cursors']
            and cfs['vram']['occupancy_sha256'] == fs['vram']['occupancy_sha256'],
            'vram_equal_to_fill00': fsha(os.path.join(ctl, 'vram.bin')) == fsha(os.path.join(runs['fill00-a'], 'vram.bin')),
            'rand_state_equal': cfs['rand_state_214ec8_58'] == fs['rand_state_214ec8_58'],
        }
    prefix_state = read_jsonl(os.path.join(runs['fill00-a'], 'state-at-100618.jsonl'))[0]
    if args.startup:
        ref['startup_cross_check'] = {'prefix_state': startup_cross_check(prefix_state, args.startup),
                                  'prefix_uploads_equal': upload_rows(ev, True) == upload_rows(read_jsonl(os.path.join(args.startup, 'events.jsonl')), False)}
    if args.rng:
        ref['rng_cross_check'] = {'prefix_rand_entries_equal': rand_rows(ev, True) == rand_rows(read_jsonl(os.path.join(args.rng, 'events.jsonl')), False),
                                  'prefix_rand_entries': len(rand_rows(ev, True))}
    with open(os.path.join(args.out, 'reference.json'), 'x') as f:
        json.dump(ref, f, indent=1)
        f.write('\n')
    return ref, state, vinfo, gw

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('runs')
    p.add_argument('--out', required=True)
    p.add_argument('--startup')
    p.add_argument('--rng')
    args = p.parse_args(argv)
    if os.path.exists(args.out):
        print('refusing existing output directory', file=sys.stderr)
        return 2
    try:
        ref, state, vinfo, gw = analyse(args)
    except Fail as exc:
        print('FAIL:', exc, file=sys.stderr)
        return 1
    c = ref['timeline_counts']
    print('timeline:', ', '.join(f'{k}={v}' for k, v in c.items()))
    print('final: allocations=%d handles=%d records=%d image_list=%s materials=%d' % (
        len(state['allocations']), len(state['handles']), len(state['records']), state['image_list'], len(state['materials'])))
    print('vram: known-required=%d unknown-required=%d' % (vinfo['known_nibbles_required'], vinfo['unknown_nibbles_required']))
    print('gs: transfers=%d draw classes=%d local-to-local=%d clut classes=%d' % (
        gw['host_to_local']['count'], len(gw['draw_classes']), len(gw['local_to_local']), len(gw['clut_classes'])))
    return 0

if __name__ == '__main__':
    sys.exit(main())
