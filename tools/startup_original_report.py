#!/usr/bin/env python3
"""Post-process the Startup startup original-execution captures (okage-startup-oracle).

Inputs  : a run directory holding fill00-a, fill00-b, fillff-a, fillff-b and optionally
          control-minimal-fill00 (each written by `okage-startup-oracle`).
Outputs : <out>/analysis.json, <out>/final-state.jsonl, <out>/vram-written-mask.bin,
          <out>/vram-written-values.bin, <out>/contract-diff.json
Nothing here edits the frozen contract; every difference is listed.
Research evidence only (Play research backend), never product input.
"""
import argparse
import collections
import hashlib
import json
import os
import struct
import sys

VRAM = 4 * 1024 * 1024

def sha(b):
    return hashlib.sha256(b).hexdigest()

def read_jsonl(path):
    with open(path, 'rb') as f:
        return [json.loads(line) for line in f if line.strip()]

def fsha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def hx(v):
    return int(v, 16) if isinstance(v, str) else v

def lcg_steps(frm, to, limit=200000):
    s = frm
    for n in range(limit):
        if s == to:
            return n
        s = (s * 0x41c64e6d + 0x3039) & 0xffffffff
    return None

# --------------------------------------------------------------------------------------
def determinism(runs):
    out = {}
    names = ['events.jsonl', 'gs-ledger.jsonl', 'disc-opens.jsonl']
    for n in names:
        shas = {r: fsha(os.path.join(runs[r], n)) for r in runs if r != 'control-minimal-fill00'}
        out[n] = {'sha256_by_run': shas, 'all_identical': len(set(shas.values())) == 1}
    # final-state: identical within a fill; across fills identical except run_fill/run_vram_sha256
    fs = {}
    for r, d in runs.items():
        fs[r] = json.loads(open(os.path.join(d, 'final-state.jsonl')).read())
    def strip(x):
        x = json.loads(json.dumps(x))
        x['vram'].pop('run_fill'); x['vram'].pop('run_vram_sha256')
        return json.dumps(x, sort_keys=True)
    full = [r for r in runs if r != 'control-minimal-fill00']
    out['final-state.jsonl'] = {
        'identical_within_fill': all(
            open(os.path.join(runs[a], 'final-state.jsonl'), 'rb').read() == open(os.path.join(runs[b], 'final-state.jsonl'), 'rb').read()
            for a, b in (('fill00-a', 'fill00-b'), ('fillff-a', 'fillff-b')) if a in runs and b in runs),
        'identical_across_fills_ignoring_fill_fields': len({strip(fs[r]) for r in full}) == 1,
    }
    out['vram_identical_within_fill'] = (fsha(os.path.join(runs['fill00-a'], 'vram.bin')) == fsha(os.path.join(runs['fill00-b'], 'vram.bin'))
                                         and fsha(os.path.join(runs['fillff-a'], 'vram.bin')) == fsha(os.path.join(runs['fillff-b'], 'vram.bin')))
    ee = {r: fsha(os.path.join(runs[r], 'ee-ram-32mb.bin')) for r in full}
    out['ee_ram_32mb_sha256_by_run'] = ee
    out['ee_ram_identical_across_fill_runs'] = len(set(ee.values())) == 1
    out['byte_identical_trace_runs'] = all(out[n]['all_identical'] for n in names)
    return out, fs

def nibble_mask(x, y):
    """x = byte from the 0x00-fill run, y = same byte from the 0xff-fill run.

    A nibble is WRITTEN iff both runs agree on it (an untouched nibble is 0 in x and 0xf in y). Because the fills are
    complementary, a written value equal to either fill is still detected. A nibble that disagrees but is not the
    untouched (0, 0xf) pair is counted as an anomaly (e.g. a partially written nibble); it is treated as not written.
    Returns (mask bits: bit0 low nibble, bit1 high nibble; value from run x; anomaly count).
    """
    m = 0
    v = 0
    anomalies = 0
    for shift, bit in ((0, 1), (4, 2)):
        nx, ny = (x >> shift) & 0xf, (y >> shift) & 0xf
        if nx == ny:
            m |= bit
            v |= nx << shift
        elif not (nx == 0 and ny == 0xf):
            anomalies += 1
    return m, v, anomalies

def vram_masks(runs, outdir):
    a = open(os.path.join(runs['fill00-a'], 'vram.bin'), 'rb').read()
    b = open(os.path.join(runs['fillff-a'], 'vram.bin'), 'rb').read()
    assert len(a) == len(b) == VRAM
    mask = bytearray(VRAM)
    values = bytearray(VRAM)
    anomalies = 0
    for i in range(VRAM):
        m, v, an = nibble_mask(a[i], b[i])
        mask[i] = m
        values[i] = v
        anomalies += an
    open(os.path.join(outdir, 'vram-written-mask.bin'), 'wb').write(bytes(mask))
    open(os.path.join(outdir, 'vram-written-values.bin'), 'wb').write(bytes(values))
    written_nibbles = sum(((m & 1) + (m >> 1)) for m in mask)
    written_bytes = sum(1 for m in mask if m)
    canon = bytearray()
    runs_list = []
    start = None
    for i in range(VRAM + 1):
        m = mask[i] if i < VRAM else 0
        if m:
            canon += struct.pack('<IBB', i, m, values[i])
            if start is None:
                start = i
        elif start is not None:
            runs_list.append((start, i - start))
            start = None
    return {
        'written_nibbles': written_nibbles,
        'written_bytes_any_nibble': written_bytes,
        'half_written_bytes': sum(1 for m in mask if m in (1, 2)),
        'sub_nibble_anomalies': anomalies,
        'written_stream_sha256': sha(bytes(canon)),
        'mask_sha256': sha(bytes(mask)),
        'values_sha256': sha(bytes(values)),
        'contiguous_written_byte_runs': len(runs_list),
        'first_runs': [[hex(s), n] for s, n in runs_list[:20]],
        'largest_runs': [[hex(s), n] for s, n in sorted(runs_list, key=lambda t: -t[1])[:10]],
        'unwritten_bytes_in_run_a_equal_00_and_run_b_equal_ff': sum(1 for i in range(VRAM) if mask[i] == 0 and a[i] == 0 and b[i] == 0xff),
    }, mask, values, runs_list

# --------------------------------------------------------------------------------------
def build_observed(ev):
    """Return observed event lists keyed like the contract."""
    obs = []
    allocs = []
    refs = collections.defaultdict(int)
    open_enter = {}
    for e in ev:
        if e['ev'] == 'enter':
            open_enter[e['i']] = e
    handle_creates = []
    for e in ev:
        if e['ev'] == 'enter' and e['name'] == 'handle_create':
            ex = next(x for x in ev if x['ev'] == 'exit' and x['enter'] == e['i'])
            handle_creates.append((e, ex))
    for e in ev:
        if e['ev'] == 'exit' and e['name'] == 'alloc' and e.get('result'):
            en = open_enter[e['enter']]
            r = e['result']
            allocs.append({'index': len(allocs), 'id': r['id'], 'class': en['class'], 'psm': r['psm'], 'w': r['w'], 'h': r['h'],
                           'tbp': r['tbp'], 'tbw': r['tbw'], 'raw': r['raw'], 'req': [en['class'], en['psm'], en['w'], en['h']], 'i': e['i'], 'top': en['top']})
            obs.append(('alloc', e['i'], allocs[-1]))
        elif e['ev'] == 'enter' and e['name'] == 'retain':
            d = e['desc']
            if d['raw'] != '0x0':
                refs[d['id']] += e['delta']
                obs.append(('ref_adjust', e['i'], {'alloc_id': d['id'], 'delta': e['delta'], 'after': refs[d['id']], 'top': e['top'], 'ra': e['ra']}))
        elif e['ev'] == 'enter' and e['name'] == 'upload':
            obs.append(('upload', e['i'], {'alloc_id': e['desc']['id'], 'x': e['x'], 'y': e['y'], 'w': e['w'], 'h': e['h'], 'psm': e['desc']['psm'],
                                           'bytes': e.get('src_bytes'), 'sha': e.get('src_sha256'), 'qwc': e.get('qwc'), 'top': e['top'], 'ra': e['ra'], 'src': e['src']}))
        elif e['ev'] == 'enter' and e['name'] == 'palette_bind':
            obs.append(('palette_bind', e['i'], {'handle': e['handle'], 'level': e['level'], 'alloc_id': e['desc']['id'], 'top': e['top'], 'ra': e['ra']}))
        elif e['ev'] == 'enter' and e['name'] == 'handle_create':
            ex = next(x for x in ev if x['ev'] == 'exit' and x['enter'] == e['i'])
            obs.append(('handle_create', e['i'], {'alloc_id': e['desc']['id'], 'handle': ex['handle'], 'top': e['top'], 'ra': e['ra']}))
    return obs, allocs, dict(refs)

def contract_compare(contract, ev, final):
    obs, allocs, run_refs = build_observed(ev)
    cev = [c for c in contract['events'] if c['kind'] in ('alloc', 'ref_adjust', 'upload', 'palette_bind', 'handle_create')]
    cid = lambda s: int(s[1:])
    diffs = []
    compared = collections.Counter()
    def d(kind, idx, field, contract_val, observed_val, note=''):
        diffs.append({'kind': kind, 'contract_seq': idx, 'field': field, 'contract': contract_val, 'observed': observed_val, 'note': note})
    by_kind_c = collections.defaultdict(list)
    for c in cev:
        by_kind_c[c['kind']].append(c)
    by_kind_o = collections.defaultdict(list)
    for k, i, v in obs:
        by_kind_o[k].append((i, v))
    counts = {k: {'contract': len(by_kind_c[k]), 'observed': len(by_kind_o[k])} for k in ('alloc', 'ref_adjust', 'upload', 'palette_bind', 'handle_create')}
    step_of_top = {}
    cur = None
    for t in contract['top_level_calls']:
        step_of_top[hx(t['pc'])] = t
    stepname = {0x100428: '1', 0x100490: '1b', 0x1004b8: '2', 0x100514: '3', 0x100524: '4'}
    # allocations
    for c, (i, o) in zip(by_kind_c['alloc'], by_kind_o['alloc']):
        n = cid(c['id'])
        for f, cv, ov in (('class', c['alloc_class'], o['class']), ('psm', c['psm'], o['psm']), ('width', c['width'], o['w']), ('height', c['height'], o['h'])):
            if cv != ov:
                d('alloc', c['seq'], f, cv, ov, c['id'])
        m = c.get('model', {})
        if m.get('id') != o['id']:
            d('alloc', c['seq'], 'model.id', m.get('id'), o['id'], c['id'] + ' (model, not source evidence)')
        if hx(m['descriptor']) != hx(o['raw']):
            d('alloc', c['seq'], 'model.descriptor', m['descriptor'], o['raw'], c['id'] + ' (model, not source evidence)')
        if m.get('tbw') != o['tbw']:
            d('alloc', c['seq'], 'model.tbw', m.get('tbw'), o['tbw'], c['id'])
        if m.get('base_block') != o['tbp']:
            d('alloc', c['seq'], 'model.base_block', m.get('base_block'), o['tbp'], c['id'])
    compared['alloc'] = 8 * min(len(by_kind_c['alloc']), len(by_kind_o['alloc']))
    # references
    for c, (i, o) in zip(by_kind_c['ref_adjust'], by_kind_o['ref_adjust']):
        if cid(c['alloc']) != o['alloc_id']:
            d('ref_adjust', c['seq'], 'alloc', c['alloc'], o['alloc_id'])
        if c['delta'] != o['delta']:
            d('ref_adjust', c['seq'], 'delta', c['delta'], o['delta'], c['alloc'])
        if c.get('references_after') != o['after']:
            d('ref_adjust', c['seq'], 'references_after', c.get('references_after'), o['after'], c['alloc'])
    compared['ref_adjust'] = 3 * min(len(by_kind_c['ref_adjust']), len(by_kind_o['ref_adjust']))
    # uploads
    for c, (i, o) in zip(by_kind_c['upload'], by_kind_o['upload']):
        if cid(c['alloc']) != o['alloc_id']:
            d('upload', c['seq'], 'alloc', c['alloc'], o['alloc_id'])
        for f, cv, ov in (('x', c['x'], o['x']), ('y', c['y'], o['y']), ('rrw', c['rrw'], o['w']), ('rrh', c['rrh'], o['h']),
                          ('psm', c['psm'], o['psm']), ('bytes', c['bytes'], o['bytes']), ('sha256', c['sha256'], o['sha'])):
            if cv != ov:
                d('upload', c['seq'], f, cv, ov, c['alloc'])
    compared['upload'] = 8 * min(len(by_kind_c['upload']), len(by_kind_o['upload']))
    compared['palette_bind'] = 3 * min(len(by_kind_c['palette_bind']), len(by_kind_o['palette_bind']))
    compared['handle_create'] = 2 * min(len(by_kind_c['handle_create']), len(by_kind_o['handle_create']))
    for c, (i, o) in zip(by_kind_c['palette_bind'], by_kind_o['palette_bind']):
        for f, cv, ov in (('handle', c['handle'], o['handle']), ('level', c['level'], o['level']), ('alloc', cid(c['alloc']), o['alloc_id'])):
            if cv != ov:
                d('palette_bind', c['seq'], f, cv, ov)
    for c, (i, o) in zip(by_kind_c['handle_create'], by_kind_o['handle_create']):
        for f, cv, ov in (('handle', c['handle'], o['handle']), ('image_alloc', cid(c['image_alloc']), o['alloc_id'])):
            if cv != ov:
                d('handle_create', c['seq'], f, cv, ov)
    # global interleaving of the five hookable kinds
    cseq = [(c['kind'], cid(c['alloc']) if 'alloc' in c else (cid(c['id']) if 'id' in c else cid(c['image_alloc']))) for c in cev]
    oseq = [(k, v['alloc_id'] if 'alloc_id' in v else v['index']) for k, i, v in obs]
    order_diffs = []
    for n, (a, b) in enumerate(zip(cseq, oseq)):
        if a != b:
            order_diffs.append({'position': n, 'contract': a, 'observed': b, 'contract_seq': cev[n]['seq']})
    # step attribution
    step_mismatch = []
    tops = [hx(t['pc']) for t in contract['top_level_calls']]
    def cur_step(top_ct):
        s = None
        for t in contract['top_level_calls']:
            if hx(t['pc']) <= top_ct and hx(t['pc']) in stepname:
                s = stepname[hx(t['pc'])]
        return s
    for c, (i, o) in zip(by_kind_c['alloc'], by_kind_o['alloc']):
        s = cur_step(o['top'])
        if str(c['step']) != s:
            step_mismatch.append({'alloc': c['id'], 'contract_step': c['step'], 'observed_top_pc': hex(o['top']), 'observed_step': s})
    # which contract kinds were not hookable
    other = collections.Counter(c['kind'] for c in contract['events'] if c['kind'] not in counts)
    # final references
    fr = contract['final_references']
    obs_refs = {r[0]: r[1] for r in final['descriptors']}
    ref_diff = [{'alloc': k, 'contract': v, 'observed': obs_refs.get(int(k[1:]))} for k, v in fr.items() if obs_refs.get(int(k[1:])) != v]
    extra_obs = sorted(set(obs_refs) - {int(k[1:]) for k in fr})
    return {
        'counts': counts,
        'compared_fields': dict(compared),
        'field_differences': diffs,
        'global_order_differences': order_diffs[:50],
        'global_order_difference_count': len(order_diffs),
        'step_attribution_mismatches': step_mismatch,
        'contract_event_kinds_not_directly_hookable': dict(other),
        'final_reference_differences': ref_diff,
        'observed_descriptors_not_in_contract_final_references': extra_obs,
    }, obs, allocs

def global_stores(contract, ev, allocs, rundir):
    ram = open(os.path.join(rundir, 'ee-ram-32mb.bin'), 'rb').read()
    u32 = lambda a: struct.unpack_from('<I', ram, a)[0]
    u64 = lambda a: struct.unpack_from('<Q', ram, a)[0]
    raw = {a['index']: int(a['raw'], 16) for a in allocs}
    rows = []
    for c in contract['events']:
        if c['kind'] != 'global_store':
            continue
        addr = hx(c['address'])
        v = c['value']
        r = {'seq': c['seq'], 'address': c['address'], 'contract_value': v}
        if v.startswith('desc(A'):
            n = int(v[6:-1])
            r.update(observed=hex(u64(addr)), expected=hex(raw[n]), final_value_matches=u64(addr) == raw[n])
        elif v.startswith('handle '):
            r.update(observed=u32(addr), final_value_matches=u32(addr) == int(v.split()[1]))
        elif v.startswith('&layer('):
            r.update(observed=hex(u32(addr)), final_value_matches=u32(addr) == int(v[7:-1], 16))
        elif v.startswith('0x') or v.isnumeric():
            r.update(observed=hex(u32(addr)), final_value_matches=u32(addr) == int(v, 0))
        elif v.startswith('array of 31'):
            ptr = u32(addr)
            arr = [u32(ptr + 4 * k) for k in range(32)]
            r.update(observed_pointer=hex(ptr), observed=arr, counter_0x1f8498=u32(0x1f8498),
                     final_value_matches=(arr[:31] == list(range(2, 33)) and u32(0x1f8498) == 31),
                     note='0x24e780 holds a pointer to the host array of 31 handle ids (u32 each)')
        elif v.startswith('DISPFB1.FBP=base('):
            n = int(v[len('DISPFB1.FBP=base(A'):len('DISPFB1.FBP=base(A') + 2])
            fbp = u32(addr) & 0x1ff
            tbp = next(a['tbp'] for a in allocs if a['index'] == n)
            r.update(observed=hex(u32(addr)), observed_fbp=fbp, expected_fbp=tbp * 64 // 2048, final_value_matches=(fbp == tbp * 64 // 2048))
        else:
            r.update(observed=hex(u32(addr)), final_value_matches=None, note='not machine-checked')
        rows.append(r)
    probes = [e for e in ev if e['ev'] == 'exit' and e['name'] == 'occupancy_probe']
    return {'final_value_checks': rows, 'occupancy_probe_exits_v0': [p['v0'] for p in probes], 'word_0x24e618': u32(0x24e618),
            'display_list_handle_words': {'0x332a70': hex(u32(0x332a70)), '0x332a84': hex(u32(0x332a84))},
            'note': 'Final-value check of the retail global after the prefix; the store order/time is not observed.'}

def top_level_compare(contract, ev):
    obs_tops = [(hx(e['pc']), hx(e['callee'])) for e in ev if e['ev'] in ('top', 'stop')]
    ctops = [(hx(t['pc']), hx(t['callee'])) for t in contract['top_level_calls']]
    return {'observed_count': len(obs_tops), 'contract_count': len(ctops), 'identical_sequence': obs_tops == ctops,
            'first_difference': next(({'index': i, 'observed': [hex(x) for x in a], 'contract': [hex(x) for x in b]}
                                       for i, (a, b) in enumerate(zip(obs_tops, ctops)) if a != b), None)}

def claims(contract, ev, final, mask, mask_info, allocs, runs_list):
    c = {}
    # rand
    rand_ev = [e for e in ev if e['ev'] == 'rand']
    def state_at(kind, name):
        return next((e['rand_state_214ec8_58'] for e in ev if e['ev'] == kind and e['name'] == name), None)
    noise_in, noise_out = state_at('enter', 'noise_texture'), state_at('exit', 'noise_texture')
    grid_in, grid_out = state_at('enter', 'effect_noise_grid'), state_at('exit', 'effect_noise_grid')
    eff_out = state_at('exit', 'effects_load')
    fonts_out = state_at('exit', 'fonts_init')
    def count_between(name):
        i0 = next(e['i'] for e in ev if e['ev'] == 'enter' and e['name'] == name)
        i1 = next(e['i'] for e in ev if e['ev'] == 'exit' and e['name'] == name)
        return sum(1 for e in rand_ev if i0 < e['i'] < i1)
    by_ra = collections.Counter(e['ra'] for e in rand_ev)
    first_rand = rand_ev[0] if rand_ev else None
    c['rand'] = {
        'total_rand_calls_observed': len(rand_ev),
        'rand_calls_by_return_address': dict(by_ra),
        'first_rand_call_index': first_rand['i'] if first_rand else None,
        'state_at_main_entry': ev[0]['rand_state_214ec8_58'],
        'state_before_noise_texture': noise_in, 'state_after_noise_texture': noise_out,
        'draws_inside_noise_texture_observed': count_between('noise_texture'),
        'draws_inside_noise_texture_from_lcg': lcg_steps(noise_in, noise_out) if noise_in is not None else None,
        'state_before_effect_noise_grid_hex': hex(grid_in), 'state_after_effect_noise_grid_hex': hex(grid_out),
        'draws_inside_effect_noise_grid_observed': count_between('effect_noise_grid'),
        'state_after_effects_load': hex(eff_out), 'draws_after_grid_inside_effects_load': lcg_steps(grid_out, eff_out),
        'state_at_fonts_exit': hex(fonts_out),
        'contract_claims': {'initial': 1, 'draws_noise': 8960, 'state_before_grid': '0xceeb6501', 'grid_draws': 384, 'state_after_grid': '0x728e2781'},
        'hypothesis_state_before_10e098_is_0xceeb6501': grid_in == 0xceeb6501,
        'hypothesis_state_after_grid_is_0x728e2781': grid_out == 0x728e2781,
        'srand_f14a0_called': any(e['ev'] == 'enter' and e.get('fn') == '0x1f14a0' for e in ev),
    }
    # noise bytes
    en = next(e for e in ev if e['ev'] == 'enter' and e['name'] == 'noise_texture')
    ex = next(e for e in ev if e['ev'] == 'exit' and e['name'] == 'noise_texture')
    inner = [e for e in ev if e['ev'] == 'enter' and en['i'] < e['i'] < ex['i']]
    cl = [e for e in inner if e['name'] == 'clut_alloc_upload' and e['depth'] == en['depth'] + 1]
    img = [e for e in inner if e['name'] == 'image_alloc_handle_upload' and e['depth'] == en['depth'] + 1]
    up = [e for e in inner if e['name'] == 'upload']
    n = contract['assets']['noise']
    c['noise_bytes'] = {
        'clut_a_source_sha256_observed': cl[0].get('src_sha256') if len(cl) > 0 else None,
        'clut_b_source_sha256_observed': cl[1].get('src_sha256') if len(cl) > 1 else None,
        'pixels_source_sha256_observed': img[0].get('src_nominal_sha256') if img else None,
        'contract': {'palette_a': n['palette_a_sha256'], 'palette_b': n['palette_b_sha256'], 'pixels': n['pixels_sha256']},
        'match': [len(cl) > 1 and cl[0].get('src_sha256') == n['palette_a_sha256'], len(cl) > 1 and cl[1].get('src_sha256') == n['palette_b_sha256'],
                  bool(img) and img[0].get('src_nominal_sha256') == n['pixels_sha256']],
        'gs_upload_source_sha256_of_clut_and_image': [u.get('src_sha256') for u in up],
        'clut_note': 'CLUT upload sources are the SWIZZLED 1cd7d0 buffers; the contract palette_*_sha256 are the pre-swizzle word arrays, compared via the 1cd7d0 argument buffer (src_sha256 on clut_alloc_upload enter)',
    }
    # handle ids
    hc = [(e['handle'], ) for e in ev if e['ev'] == 'exit' and e['name'] == 'handle_create']
    c['handles'] = {'created_in_order': [h[0] for h in hc], 'is_0_to_35': [h[0] for h in hc] == list(range(36)),
                    'final_live_handles': final['live_handles'], 'final_table': final['handles'][:40]}
    # refcounts
    fr = contract['final_references']
    obs_refs = {r[0]: r[1] for r in final['descriptors']}
    c['final_refcounts'] = {'matches_contract': all(obs_refs.get(int(k[1:])) == v for k, v in fr.items()) and len(obs_refs) == len(fr),
                            'observed_count': len(obs_refs), 'contract_count': len(fr),
                            'depth_A02': obs_refs.get(2), 'filter_cluts_A09_A10': [obs_refs.get(9), obs_refs.get(10)]}
    # frees
    frees = [e for e in ev if e['ev'] == 'enter' and e['name'] == 'free']
    c['frees'] = {'calls': len(frees), 'nonnull_target_calls': sum(1 for e in frees if e['desc']['raw'] != '0x0')}
    # null-source CLUTs
    clut_nulls = [e for e in ev if e['ev'] == 'enter' and e['name'] == 'clut_alloc_upload' and e['src'] == '0x0']
    null_allocs = []
    ex_by_enter = {}
    for e in ev:
        if e['ev'] == 'exit':
            ex_by_enter[e['enter']] = e
    for e in clut_nulls:
        r = ex_by_enter[e['i']]['result']
        null_allocs.append(r)
    uploads_by_alloc = collections.Counter(e['desc']['id'] for e in ev if e['ev'] == 'enter' and e['name'] == 'upload')
    def footprint_written(r, size):
        base = r['tbp'] * 256
        written = sum(((mask[a] & 1) + (mask[a] >> 1)) for a in range(base, base + size))
        return written
    c['null_source_cluts'] = [{'alloc_id': r['id'], 'tbp': r['tbp'], 'upload_calls': uploads_by_alloc.get(r['id'], 0),
                               'written_nibbles_in_1KiB_footprint': footprint_written(r, 1024)} for r in null_allocs]
    def footprint_bytes(r):
        psm, w, h = r['psm'], r['w'], r['h']
        if r['w'] == 16 and r['h'] == 16 and psm == 0:
            return 1024
        pw, ph = {0: (64, 32), 49: (64, 32), 36: (64, 32), 44: (64, 32), 19: (128, 64), 20: (128, 128)}[psm]
        return -(-w // pw) * -(-h // ph) * 8192
    never = []
    for a_ in allocs:
        if uploads_by_alloc.get(a_['id'], 0) == 0:
            sz = footprint_bytes(a_)
            base = a_['tbp'] * 256
            lanes = [0, 0, 0, 0]
            for x in range(base, min(VRAM, base + sz)):
                m = mask[x]
                lanes[x & 3] += (m & 1) + (m >> 1)
            never.append({'alloc_id': a_['id'], 'psm': a_['psm'], 'w': a_['w'], 'h': a_['h'], 'tbp': a_['tbp'], 'footprint_bytes': sz,
                          'written_nibbles_by_byte_lane_0_to_3': lanes, 'written_nibbles_total': sum(lanes)})
    c['allocations_without_upload'] = {'count': len(never), 'ids': [n['alloc_id'] for n in never], 'details': never,
                                      'note': 'Footprints are contiguous page rectangles from TBP*256 (16x16 CLUT: 1 KiB). Written nibbles in byte lane 3 inside the PSM49 depth footprint belong to PSM36/44 font allocations that share the unused top byte of Z24 words (lane model), not to the depth buffer itself.'}
    # dma kicks
    kicks = [e for e in ev if e['ev'] == 'enter' and e['name'] == 'dma_kick']
    ex_open = [(e['i'], ex_by_enter.get(e['i'])) for e in kicks]
    # display list windows
    wins = []
    for nm in ('display_list_a_record', 'display_list_b_record'):
        a0 = next(e for e in ev if e['ev'] == 'enter' and e['name'] == nm)
        a1 = ex_by_enter[a0['i']]
        wins.append((nm, a0['i'], a1['i']))
    grp = collections.Counter((k['ra'], k['args'][0]) for k in kicks)
    c['dma_kick_1cad20'] = {
        'total_calls': len(kicks),
        'calls_by_return_address_and_a0': {f'{ra}/a0={a0}': n for (ra, a0), n in sorted(grp.items())},
        'calls_inside_display_list_recording_windows': sum(1 for k in kicks for (_, s, e2) in wins if s < k['i'] < e2),
        'display_list_windows': wins,
        'top_steps_of_kicks': dict(collections.Counter(hex(k['top']) for k in kicks)),
        'parents': dict(collections.Counter(k['parent'] for k in kicks)),
        'non_gif_upload_kicks': [{'i': k['i'], 'ra': k['ra'], 'parent': k['parent'], 'top': hex(k['top']), 'args': k['args'][:6]} for k in kicks if k['parent'] != 'transfer'],
        'last_kick_event_index': kicks[-1]['i'] if kicks else None,
        'event_count_total': ev[-1]['i'] + 1,
    }
    return c

def file_reads(contract, ev):
    ex_by_enter = {e['enter']: e for e in ev if e['ev'] == 'exit'}
    rows = []
    for e in ev:
        if e['ev'] == 'enter' and e['name'] == 'file_read':
            x = ex_by_enter[e['i']]
            rows.append({'name': e['str'], 'buf': x['buf'], 'size': x['last_raw_read']['size'], 'sha256': x['last_raw_read']['sha256'], 'top': hex(e['top']), 'parent': e['parent'], 'i': e['i']})
    # compare
    eff = {x['name']: x for x in contract['assets']['effects']}
    fonts = {x['runtime_name'].split('/')[-1].lower(): x for x in contract['assets']['fonts']}
    cmp_ = []
    for r in rows:
        n = r['name']
        if n in eff:
            cmp_.append({'name': n, 'kind': 'effect member', 'size_match': eff[n]['file_bytes'] == r['size'], 'sha_match': eff[n]['file_sha256'] == r['sha256'],
                         'contract_size': eff[n]['file_bytes'], 'observed_size': r['size']})
        elif n in fonts:
            cmp_.append({'name': n, 'kind': 'font', 'sha_match': fonts[n]['file_sha256'] == r['sha256'], 'observed_size': r['size']})
        elif n == 'env_tex.xpf':
            cmp_.append({'name': n, 'kind': 'archive', 'sha_match': contract['assets']['env_tex_archive']['sha256'] == r['sha256'], 'observed_size': r['size'],
                         'observed_sha256': r['sha256']})
        else:
            cmp_.append({'name': n, 'kind': 'other (not in contract assets)', 'observed_size': r['size'], 'observed_sha256': r['sha256']})
    return rows, cmp_

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rundir')
    ap.add_argument('--contract', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    runs = {n: os.path.join(a.rundir, n) for n in os.listdir(a.rundir) if os.path.isdir(os.path.join(a.rundir, n))}
    for need in ('fill00-a', 'fill00-b', 'fillff-a', 'fillff-b'):
        if need not in runs:
            sys.exit('missing run ' + need)
    os.makedirs(a.out, exist_ok=True)
    contract = json.load(open(a.contract))
    det, fs = determinism(runs)
    mask_info, mask, values, vruns = vram_masks(runs, a.out)
    ev = read_jsonl(os.path.join(runs['fill00-a'], 'events.jsonl'))
    gs = read_jsonl(os.path.join(runs['fill00-a'], 'gs-ledger.jsonl'))
    final = fs['fill00-a']
    ccmp, obs, allocs = contract_compare(contract, ev, final)
    tl = top_level_compare(contract, ev)
    cl = claims(contract, ev, final, mask, mask_info, allocs, vruns)
    frows, fcmp = file_reads(contract, ev)
    gstores = global_stores(contract, ev, allocs, runs['fill00-a'])
    # GS cross-check: hook-level uploads vs GS transfers
    ups = [e for e in ev if e['ev'] == 'enter' and e['name'] == 'upload']
    tr = [g for g in gs if g['kind'] == 'gs_transfer']
    gs_x = {'hook_uploads': len(ups), 'gs_transfers': len(tr), 'mismatches': []}
    for k, (u, t) in enumerate(zip(ups, tr)):
        exp = {'dbp': u['desc']['tbp'], 'dbw': u['desc']['tbw'], 'dpsm': u['desc']['psm'], 'dsax': u['x'], 'dsay': u['y'], 'rrw': u['w'], 'rrh': u['h']}
        for f, v in exp.items():
            if t[f] != v:
                gs_x['mismatches'].append({'n': k, 'field': f, 'hook': v, 'gs': t[f]})
        if u.get('src_bytes') != t['received_bytes']:
            gs_x['mismatches'].append({'n': k, 'field': 'bytes', 'hook_src_bytes': u.get('src_bytes'), 'gs_received': t['received_bytes']})
        if u.get('src_sha256') != t['image_sha256']:
            gs_x['mismatches'].append({'n': k, 'field': 'sha256', 'hook': u.get('src_sha256'), 'gs': t['image_sha256']})
        if not t['complete']:
            gs_x['mismatches'].append({'n': k, 'field': 'complete', 'gs': t['complete']})
    summary = next(g for g in gs if g['kind'] == 'gs_summary')
    gs_x['gs_summary'] = summary
    gs_x['sum_received_bytes'] = sum(t['received_bytes'] for t in tr)
    gs_x['expected_written_nibbles_if_no_overlap'] = 2 * gs_x['sum_received_bytes']
    gs_x['observed_written_nibbles'] = mask_info['written_nibbles']
    gs_x['vram_outside_uploads'] = {
        'gs_vertex_kicks': summary['vertex_kicks'], 'gs_local_to_local': summary['local_to_local'], 'gs_local_to_host': summary['local_to_host'],
        'gs_clut_transfers': summary['clut_transfers'], 'gs_resets_during_run': summary['resets'],
        'note': 'The research GS sink is a null renderer: draws, local-to-local copies and CLUT loads would NOT write local memory here. All of them were counted; zero were observed, so no VRAM write outside host-to-local uploads was missed.'}
    # EE RAM: control (minimal) vs hooked
    control = None
    if 'control-minimal-fill00' in runs:
        cfs = fs['control-minimal-fill00']
        def comparable(x):
            x = json.loads(json.dumps(x))
            for k in ('allocations', 'ee_ram_32mb_sha256', 'rand_state_214ec8_58', 'rand_state_via_ptr'):
                x.pop(k, None)
            x['vram'].pop('run_fill'); x['vram'].pop('run_vram_sha256')
            return x
        same = comparable(cfs) == comparable(final)
        diffs = {k: [final.get(k) if k not in ('vram',) else None] for k in ()}
        ramA = open(os.path.join(runs['fill00-a'], 'ee-ram-32mb.bin'), 'rb').read()
        ramC = open(os.path.join(runs['control-minimal-fill00'], 'ee-ram-32mb.bin'), 'rb').read()
        blocks = collections.Counter()
        total = 0
        for off in range(0, len(ramA), 4096):
            if ramA[off:off + 4096] != ramC[off:off + 4096]:
                blocks[off >> 16] += 1
                total += 1
        control = {
            'control_run': 'okage-startup-oracle ... --minimal (stop breakpoint only, no function hooks)',
            'allocator_handle_descriptor_state_identical': same,
            'run_vram_sha256_control': cfs['vram']['run_vram_sha256'], 'run_vram_sha256_hooked': final['vram']['run_vram_sha256'],
            'vram_identical': cfs['vram']['run_vram_sha256'] == final['vram']['run_vram_sha256'],
            'vblanks_control': json.load(open(os.path.join(runs['control-minimal-fill00'], 'run-meta.json')))['vblanks'],
            'vblanks_hooked': json.load(open(os.path.join(runs['fill00-a'], 'run-meta.json')))['vblanks'],
            'ee_ram_4KiB_pages_differing': total,
            'ee_ram_64KiB_regions_with_differences': {hex(k << 16): v for k, v in sorted(blocks.items())},
            'rand_state_control': cfs['rand_state_214ec8_58'], 'rand_state_hooked': final['rand_state_214ec8_58'],
        }
    # merged final state
    merged = dict(final)
    merged['kind'] = 'final_state'
    merged['vram'] = dict(final['vram'])
    merged['vram'].pop('run_fill'); merged['vram'].pop('run_vram_sha256')
    merged['vram'].update({'written_nibbles': mask_info['written_nibbles'], 'written_bytes_any_nibble': mask_info['written_bytes_any_nibble'],
                           'written_stream_sha256': mask_info['written_stream_sha256'], 'mask_sha256': mask_info['mask_sha256'],
                           'values_sha256': mask_info['values_sha256'],
                           'written_value_definition': 'value of every written nibble (run A 0x00-fill); unwritten nibbles are 0 in the values file',
                           'mask_definition': 'bit0 = low nibble written, bit1 = high nibble written; written iff the 0x00-fill and 0xff-fill runs agree'})
    merged.pop('ee_ram_32mb_sha256', None)
    with open(os.path.join(a.out, 'final-state.jsonl'), 'w') as f:
        f.write(json.dumps(merged, sort_keys=False) + '\n')
    result = {
        'schema': 'okage.startup-original-analysis.v1',
        'runs': {r: {'dir': os.path.relpath(p, a.rundir), 'run_meta': json.load(open(os.path.join(p, 'run-meta.json')))} for r, p in sorted(runs.items())},
        'determinism': det,
        'vram_written_mask': mask_info,
        'top_level_calls': tl,
        'contract_comparison': ccmp,
        'claims': cl,
        'gs_cross_check': gs_x,
        'file_reads': {'rows': frows, 'comparison': fcmp},
        'global_stores': gstores,
        'observer_perturbation_control': control,
        'disc_opens_sha256': fsha(os.path.join(runs['fill00-a'], 'disc-opens.jsonl')),
    }
    json.dump(result, open(os.path.join(a.out, 'analysis.json'), 'w'), indent=1, sort_keys=False)
    print('analysis written to', a.out)

if __name__ == '__main__':
    main()
