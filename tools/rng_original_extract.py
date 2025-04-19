#!/usr/bin/env python3
"""Extract the RNG original startup RNG/effect-preparation reference from okage-startup-oracle --rng-products runs.

Inputs : RUN_A RUN_B (two --rng-products captures, must be byte-identical apart from run-meta wall time),
         --contract docs/startup-contract.json (top-level step pcs), --functions <Ghidra functions.tsv>
         (caller-function spans), optional --startup <Startup fill00-a run> (observer-perturbation cross-check).
Outputs: OUT/rng.jsonl, OUT/effects.json   (exactly the native comparison schema, see tools/rng_compare.py)
         OUT/rng-detail.jsonl              (per draw: ra, caller, step, state before/after, template attribution)
         OUT/reference.json                (phases, checkpoints, consumer runs, requests incl. runtime ids,
                                            share outcomes, structure addresses, determinism, provenance hashes)
Every value comes from the unchanged retail program as executed under the Play research VM: RNG values are the
observed rand() return registers and state words; products are read from EE RAM (after 0x10bc78 and at the stop).
No native code or data is consulted. Research evidence only, never product input.
"""
import argparse
import bisect
import hashlib
import json
import os
import struct
import sys

LCG_A, LCG_C = 0x41c64e6d, 0x3039
RAND_REENT = 0x214ec8          # *(0x2151b4): libc reent; rand state word at +0x58
GRID_PTR = 0x24e9fc            # 0x10e098: int[8*8*6] from 0x103b78, value rand()%7
CACHE_DEF, CACHE_ELEM = 0x24e790, 0x24e898
TEMPLATE_TABLE_PTR = 0x1f849c  # pointer to the job/template pointer table; EFD definition id d lives at slot d+0x54
ELEMENT_STRIDE = 0x80          # job element record: +0x50 element id, +0x64 geometry piece chain
PROCEDURAL_FACTORIES = {'factory_noise_shape': 0x10e138, 'factory_rays': 0x10dbc0,
                        'factory_surface': 0x111768, 'factory_spray': 0x10e918}
PHASE_FRAME = (('noise_texture', 'noise'), ('effect_noise_grid', 'effect-grid'), ('efd_import', 'effect-template'))
EXPECTED_COUNTS = {'noise': 8960, 'effect-grid': 384, 'effect-template': 160}
VIF_STRIDES = {0x60: 4, 0x68: 12, 0x6c: 16, 0x6e: 4, 0x75: 4, 0x79: 6}

class ExtractError(ValueError):
    pass

def need(cond, msg):
    if not cond:
        raise ExtractError(msg)

def fsha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def hx(v):
    return int(v, 16) if isinstance(v, str) else int(v)

def f32hex(x):
    b = struct.pack('<f', x)
    need(struct.unpack('<f', b)[0] == x, f'value {x!r} not exactly representable in float32')
    return '0x%08x' % struct.unpack('<I', b)[0]

class Ram:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.m = f.read()
        need(len(self.m) == 0x2000000, f'{path}: EE RAM snapshot must be 32 MiB')
        self.sha256 = hashlib.sha256(self.m).hexdigest()

    def w(self, a):
        a &= 0x1fffffff
        need(0 <= a <= len(self.m) - 4, f'RAM word out of range 0x{a:x}')
        return struct.unpack_from('<I', self.m, a)[0]

    def b(self, a, n):
        a &= 0x1fffffff
        need(0 <= a and n >= 0 and a + n <= len(self.m), f'RAM range out of range 0x{a:x}+{n}')
        return self.m[a:a + n]

def unpack_records(packet, wanted, fmt):
    """Bounded retail VIF packet: DMA tag qword, VIF codes, UNPACK payloads padded to words."""
    need(len(packet) >= 32 and len(packet) % 16 == 0, 'packet extent')
    word = lambda at: struct.unpack_from('<I', packet, at)[0]
    need((word(0) & 0xffff) * 16 + 32 == len(packet), 'packet DMA qword count')
    at, records = 16, []
    while at < len(packet) - 16:
        command = word(at)
        at += 4
        op, count = command >> 24, (command >> 16) & 255
        if op in (0x79, 0x75):
            need(command & 0xffff == {0x79: 0x8001, 0x75: 0x41aa}[op], f'VIF destination {command:08x}')
        if op in VIF_STRIDES:
            size = (count or 256) * VIF_STRIDES[op]
        elif op == 0x20:
            size = 16
        elif op in (0, 1, 4, 0x14, 0x17):
            size = 0
        else:
            raise ExtractError(f'unreviewed VIF command {command:08x}')
        padded = (size + 3) // 4 * 4
        need(at + padded <= len(packet) - 16, 'VIF payload extent')
        if op == wanted:
            records.append(list(struct.iter_unpack(fmt, packet[at:at + size])))
        at += padded
    return records

def packet(ram, p):
    n = (ram.w(p) & 0xffff) * 16 + 32
    need(n <= 0x10000, 'packet too large')
    return ram.b(p, n)

def decode_geometry(ram, chain):
    """Geometry piece chain -> native-format triangle-list vertices + tags (see the RNG original-reference contract).

    Piece (0x30 bytes): +0 next, +4 position packet (V3-16, 1/64 units), +8 UV packet (V2-16 /32768; stale when
    the UV record count is 0), +0xc colour packet (V4-8), +0x10 strip packet (V4-8 corners: position, normal,
    colour, uv indices), +0x14 position-tag bytes, +0x18 UV-tag bytes, +0x1c counts (lo16 position tags, hi16 UV tags).
    """
    pieces, raw = [], []
    g = chain
    while g:
        need(len(pieces) < 64, 'piece chain too long')
        pieces.append(g)
        g = ram.w(g)
    verts, ptags, utags = [], [], []
    for g in pieces:
        counts = ram.w(g + 0x1c)
        n_ptag, n_utag = counts & 0xffff, counts >> 16
        pos = [v for rec in unpack_records(packet(ram, ram.w(g + 4)), 0x79, '<3h') for v in rec]
        col = [v for rec in unpack_records(packet(ram, ram.w(g + 0xc)), 0x6e, '<4B') for v in rec]
        textured = n_utag != 0
        uvs = [v for rec in unpack_records(packet(ram, ram.w(g + 8)), 0x75, '<2H') for v in rec] if textured else None
        pt = list(ram.b(ram.w(g + 0x14), n_ptag)) if n_ptag else None
        ut = list(ram.b(ram.w(g + 0x18), n_utag)) if n_utag else None
        need(pt is None or len(pt) == len(pos), 'position tag count differs from position records')
        need(ut is None or len(ut) == len(uvs), 'UV tag count differs from UV records')
        strips = unpack_records(packet(ram, ram.w(g + 0x10)), 0x6e, '<4B')
        raw.append(dict(piece=g, positions=len(pos), colors=len(col), uvs=len(uvs) if textured else 0,
                        position_tags=n_ptag, uv_tags=n_utag, strips=[len(s) for s in strips]))
        for strip in strips:
            need(len(strip) >= 3, 'strip shorter than 3 corners')
            corners = []
            for p, normal, c, t in strip:
                need(normal == 0 and p < len(pos) and c < len(col), 'corner index out of range')
                need((t < len(uvs)) if textured else t == 0, 'UV index out of range')
                x, y, z = pos[p]
                r, gg, bb, a = col[c]
                u, v = (uvs[t][0] / 32768, uvs[t][1] / 32768) if textured else (0.0, 0.0)
                corners.append(([f32hex(x / 64), f32hex(y / 64), f32hex(z / 64), f32hex(1.0),
                                 f32hex(float(r)), f32hex(float(gg)), f32hex(float(bb)), f32hex(float(a)),
                                 f32hex(u), f32hex(v), f32hex(0.0), f32hex(0.0)],
                                pt[p] if pt is not None else 0, ut[t] if ut is not None else 0))
            for i in range(len(corners) - 2):
                for vtx, ptag, utag in corners[i:i + 3]:
                    verts.append(vtx)
                    ptags.append(ptag)
                    utags.append(utag)
    return verts, ptags, utags, raw

def load_events(run):
    with open(os.path.join(run, 'events.jsonl'), 'rb') as f:
        return [json.loads(line) for line in f if line.strip()]

def function_index(path):
    starts, rows = [], []
    with open(path) as f:
        header = f.readline().rstrip('\n').split('\t')
        need(header[:3] == ['address', 'name', 'span_bytes'], 'functions.tsv header')
        for line in f:
            a, name, span = line.rstrip('\n').split('\t')[:3]
            rows.append((int(a, 16), name, int(span)))
    rows.sort()
    starts = [r[0] for r in rows]
    def find(addr):
        k = bisect.bisect_right(starts, addr) - 1
        need(k >= 0 and addr < rows[k][0] + rows[k][2], f'no function contains 0x{addr:x}')
        return rows[k]
    return find

def extract(run_a, run_b, contract_path, functions_path, startup_run=None):
    meta_a = json.load(open(os.path.join(run_a, 'run-meta.json')))
    meta_b = json.load(open(os.path.join(run_b, 'run-meta.json')))
    for meta in (meta_a, meta_b):
        need(meta.get('rng_products') is True and meta.get('minimal') is False, 'runs must be --rng-products captures')
        need(meta.get('rand_pending_at_stop') is False and meta.get('open_frames_at_stop') == ['main'], 'stop state')
    determinism = {}
    for name in ('events.jsonl', 'ee-ram-after-10bc78.bin', 'ee-ram-32mb.bin', 'final-state.jsonl',
                 'gs-ledger.jsonl', 'disc-opens.jsonl', 'vram.bin'):
        a, b = fsha(os.path.join(run_a, name)), fsha(os.path.join(run_b, name))
        determinism[name] = dict(sha256=a, identical=a == b)
        need(a == b, f'determinism: {name} differs between runs')
    events = load_events(run_a)
    contract = json.load(open(contract_path))
    step_of = {hx(c['pc']): c['seq'] for c in contract['top_level_calls']}
    find_fn = function_index(functions_path)

    # ---- walk events with an explicit frame stack ------------------------------------------
    frames, draws, pending = [], [], None
    defs, requests, shares, constructions, cur_def, cur_range = [], [], [], [], None, None
    grid_exit = None
    snapshot_name = None
    stop = None
    efd_reads = []
    for e in events:
        ev = e['ev']
        if ev == 'enter':
            frames.append(e)
            name = e['name']
            if name == 'efd_define':
                cur_def = e['def']
                defs.append(dict(id=e['def']['id'], flags=e['def']['flags'], elements=e['def']['elements']))
            elif name == 'efd_range':
                need(e['def'] is not None and cur_def is not None and e['def']['id'] == cur_def['id'], 'range def')
                cur_range = e
            elif name == 'template_request':
                need(cur_def is not None, 'request outside a definition')
                via = e['parent']
                need(0 < e['shape'] < 66, f'request shape {e["shape"]} outside the 66-entry cache')
                need(via in ('efd_define', 'efd_range'), f'unexpected request caller {via}')
                if via == 'efd_range':
                    source = e['elem'] - cur_range['base'] + cur_range['start']
                    need(cur_range['start'] <= source <= cur_range['end'], 'range source id')
                else:
                    source = e['elem']
                req = dict(index=len(requests), event=e['i'], def_id=cur_def['id'], source_elem=source,
                           runtime_elem=e['elem'], shape=e['shape'], texture=e['texture'], via=via,
                           constructed=e['cache_def_before'] == 0,
                           cache_before=[e['cache_def_before'], e['cache_elem_before']],
                           slot=e['current_slot'], job=e['current']['job'], count_before=e['current']['count'],
                           factory=None, draws=0, first_draw=None, share=None)
                need(req['slot'] == cur_def['id'] + 0x54, 'current template slot is not def id + 0x54')
                requests.append(req)
            elif name.startswith('factory_') and e['parent'] == 'template_request':
                req = requests[-1]
                need(req['factory'] is None and req['constructed'], 'factory without empty cache slot')
                req['factory'] = name
            elif name == 'template_share':
                req = requests[-1]
                need(e['parent'] == 'template_request' and not req['constructed'], 'share on a constructing request')
                need(e['owner_slot'] == req['cache_before'][0] + 0x54 and e['owner_elem'] == req['cache_before'][1]
                     and e['elem'] == req['runtime_elem'], 'share arguments')
                req['share'] = dict(owner_slot=e['owner_slot'], owner_elem=e['owner_elem'],
                                    owner_count=e['owner']['count'], current_count_before=e['current']['count'])
        elif ev == 'exit':
            need(frames and frames[-1]['i'] == e['enter'], f'unbalanced exit at event {e["i"]}')
            f = frames.pop()
            name = f['name']
            if name == 'template_request':
                req = requests[-1]
                req['count_after'] = e['current']['count']
            elif name == 'template_share':
                requests[-1]['share']['current_count_after'] = e['current']['count']
            elif name == 'efd_range':
                cur_range = None
            elif name == 'efd_define':
                cur_def = None
            elif name == 'effect_noise_grid':
                grid_exit = e
            elif name == 'effect_script_init':
                snapshot_name = e['snapshot']
                need(fsha(os.path.join(run_a, snapshot_name)) == e['snapshot_sha256'], 'snapshot hash')
        elif ev == 'rand':
            need(pending is None and e['n'] == len(draws), 'rand order')
            pending = e
        elif ev == 'rand_exit':
            need(pending is not None and e['n'] == pending['n'] and e['ra'] == pending['ra'], 'rand exit pairing')
            s0, s1 = pending['state'], e['state']
            need(pending['rand_ptr_2151b4'] == RAND_REENT and e['rand_ptr_2151b4'] == RAND_REENT, 'rand reent pointer')
            need(s1 == (s0 * LCG_A + LCG_C) & 0xffffffff, f'draw {e["n"]}: state transition is not the libc LCG')
            need(e['v0'] == s1 & 0x7fffffff and e['v0_hi'] == 0, f'draw {e["n"]}: v0 != state & 0x7fffffff')
            names = pending['frames']
            phase = None
            for frame, p in PHASE_FRAME:
                if frame in names:
                    need(phase is None, 'ambiguous phase')
                    phase = p
            need(phase is not None, f'draw {e["n"]} outside a known consumer: {names}')
            ra = hx(pending['ra'])
            fn = find_fn(ra - 8)
            d = dict(i=e['n'], phase=phase, ra='0x%x' % ra, call_site='0x%x' % (ra - 8),
                     caller_function='0x%x' % fn[0], caller_name=fn[1], hooked_parent=pending['parent'],
                     frames=names, top_pc='0x%x' % pending['top'], top_step=step_of.get(pending['top']),
                     state_before=s0, state=s1, value=e['v0'])
            need(d['top_step'] is not None, 'draw outside a contract top-level step')
            if phase == 'effect-template':
                req = requests[-1]
                need(frames and frames[-1]['name'] == req['factory'] == pending['parent'],
                     'template draw not directly inside the current request factory')
                if req['first_draw'] is None:
                    req['first_draw'] = d['i']
                req['draws'] += 1
                d.update(request=req['index'], def_id=req['def_id'], source_elem=req['source_elem'],
                         runtime_elem=req['runtime_elem'], shape=req['shape'], factory=req['factory'])
            draws.append(d)
            pending = None
        if ev == 'exit' and e['name'] == 'file_read_raw' and e.get('file', '').lower() == 'tes.efd':
            efd_reads.append(dict(sha256=e['sha256'], bytes=e['bytes_read'], buf=e['buf']))
        if ev == 'stop':
            need(stop is None, 'two stop events')
            stop = e
    need(pending is None and not [f for f in frames if f['name'] != 'main'], 'open frames at stop')
    need(len(draws) == 9504, f'expected 9504 draws, got {len(draws)}')

    # ---- phases and checkpoints --------------------------------------------------------------
    boundaries, k = [], 0
    for phase in ('noise', 'effect-grid', 'effect-template'):
        n = 0
        while k + n < len(draws) and draws[k + n]['phase'] == phase:
            n += 1
        need(n == EXPECTED_COUNTS[phase], f'phase {phase}: {n} contiguous draws')
        boundaries.append(dict(phase=phase, first=k, last=k + n - 1, count=n,
                               state_in=draws[k]['state_before'], state_out=draws[k + n - 1]['state']))
        k += n
    need(k == len(draws), 'draws after the last phase')
    need(draws[0]['state_before'] == 1, 'initial rand state is not 1')

    # ---- products ------------------------------------------------------------------------------
    s1 = Ram(os.path.join(run_a, snapshot_name))
    s2 = Ram(os.path.join(run_a, 'ee-ram-32mb.bin'))
    grid_ptr = s1.w(GRID_PTR)
    need(grid_ptr == s2.w(GRID_PTR) and grid_ptr == hx(grid_exit['grid_ptr']), 'grid pointer')
    grid = list(struct.unpack('<384I', s1.b(grid_ptr, 384 * 4)))
    need(grid == list(struct.unpack('<384I', s2.b(grid_ptr, 384 * 4))), 'grid changed between 0x10bc78 exit and stop')
    need(grid == grid_exit['grid'], 'grid differs from the 0x10e098 exit observation')
    gd = [d for d in draws if d['phase'] == 'effect-grid']
    need(grid == [d['value'] % 7 for d in gd], 'grid is not rand()%7 of the grid draws')
    cache = []
    for shape in range(66):
        a = (s2.w(CACHE_DEF + 4 * shape), s2.w(CACHE_ELEM + 4 * shape))
        need(a == (s1.w(CACHE_DEF + 4 * shape), s1.w(CACHE_ELEM + 4 * shape)), 'cache changed after 0x10bc78')
        if a != (0, 0):
            cache.append([shape, a[0], a[1]])
    table = s1.w(TEMPLATE_TABLE_PTR)
    need(table == s2.w(TEMPLATE_TABLE_PTR), 'template table moved')

    def element(ram, def_id, index):
        job = ram.w(table + (def_id + 0x54) * 4)
        need(ram.w(job) == def_id and index < ram.w(job + 8), 'job identity/extent')
        return ram.w(job + 0xc) + index * ELEMENT_STRIDE

    templates, share_checks = [], []
    owner_geometry = {}
    for req in requests:
        if req['constructed']:
            need(req['factory'] is not None and req['count_after'] == req['count_before'] + 1,
                 'construction must add exactly one element')
        else:
            need(req['share'] is not None, 'occupied cache slot without 0x113508')
        if req['count_after'] == req['count_before']:
            req['element_index'] = None
            req['geometry'] = None
            req['share']['found_owner_element'] = False
            continue
        el = element(s1, req['def_id'], req['count_before'])
        need(s1.w(el + 0x50) == req['runtime_elem'], 'element id at request index')
        geom = s1.w(el + 0x64)
        req['element_index'] = req['count_before']
        req['geometry'] = '0x%x' % geom
        if req['constructed']:
            owner_geometry[req['shape']] = (geom, req)
        else:
            sh = req['share']
            added = sh['current_count_after'] - sh['current_count_before']
            need(added in (0, 1) and req['count_after'] - req['count_before'] == added, 'share element accounting')
            sh['found_owner_element'] = added == 1
            if req['shape'] in owner_geometry and added:
                sh['same_geometry_as_construction'] = geom == owner_geometry[req['shape']][0]
    order = 0
    for req in requests:
        if not req['constructed'] or req['factory'] not in PROCEDURAL_FACTORIES:
            continue
        el1, el2 = element(s1, req['def_id'], req['count_before']), element(s2, req['def_id'], req['count_before'])
        need(s1.w(el1 + 0x64) == s2.w(el2 + 0x64), 'geometry chain moved before the stop')
        v1 = decode_geometry(s1, s1.w(el1 + 0x64))
        v2 = decode_geometry(s2, s2.w(el2 + 0x64))
        need(v1[:3] == v2[:3], f'shape {req["shape"]} geometry changed between 0x10bc78 exit and stop')
        verts, ptags, utags, raw = v1
        templates.append(dict(shape=req['shape'], owner=[req['def_id'], req['source_elem']], order=order,
                              draws=req['draws'], vertices=verts, position_tags=ptags, uv_tags=utags))
        req['template_order'] = order
        req['pieces'] = raw
        order += 1
    effects = dict(grid=grid, templates=templates, cache_owners=cache,
                   requests=[dict(**{'def': r['def_id']}, elem=r['source_elem'], shape=r['shape'],
                                  constructed=r['constructed']) for r in requests])

    # ---- consumer runs within effect-template ----------------------------------------------------
    runs = []
    for d in draws:
        if d['phase'] != 'effect-template':
            continue
        if not runs or runs[-1]['request'] != d['request']:
            runs.append(dict(request=d['request'], def_id=d['def_id'], source_elem=d['source_elem'],
                             runtime_elem=d['runtime_elem'], shape=d['shape'], factory=d['factory'],
                             factory_address='0x%x' % PROCEDURAL_FACTORIES[d['factory']],
                             first=d['i'], draws=0, state_in=d['state_before'], call_sites={}))
        r = runs[-1]
        r['draws'] += 1
        r['state_out'] = d['state']
        r['call_sites'][d['call_site']] = r['call_sites'].get(d['call_site'], 0) + 1

    cross = None
    if startup_run:
        old = [x for x in load_events(startup_run) if x['ev'] == 'rand']
        cross = dict(startup_run=startup_run, startup_events_sha256=fsha(os.path.join(startup_run, 'events.jsonl')),
                     rand_entries_identical=[(hx(x['ra']), x['state'], x['top']) for x in old] ==
                     [(hx(d['ra']), d['state_before'], hx(d['top_pc'])) for d in draws])
        for name in ('gs-ledger.jsonl', 'vram.bin', 'disc-opens.jsonl'):
            cross[name + '_identical'] = fsha(os.path.join(startup_run, name)) == determinism[name]['sha256']

    summary = dict(
        schema='okage.rng-original-reference.v1',
        scope='Original cold-start rand() calls and products up to (not executing) EE PC 0x100618, from unchanged '
              'retail SCUS_971.29 under the Play research VM. Research evidence only.',
        counts={p: EXPECTED_COUNTS[p] for p in EXPECTED_COUNTS}, total_draws=len(draws),
        phases=boundaries, final_state=draws[-1]['state'],
        stop_rand_state=stop['rand_state_214ec8_58'],
        noise_call_sites=_count([d['call_site'] for d in draws if d['phase'] == 'noise']),
        grid_call_sites=_count([d['call_site'] for d in draws if d['phase'] == 'effect-grid']),
        template_runs=runs,
        efd=dict(reads=efd_reads, definitions_in_file_order=len(defs),
                 definition_ids_in_file_order=[x['id'] for x in defs],
                 definitions_with_reference_flag=sum(1 for x in defs if hx(x['flags']) & 0x800)),
        requests=[{k: v for k, v in r.items() if k not in ('event',)} for r in requests],
        requests_total=len(requests), constructions=sum(r['constructed'] for r in requests),
        procedural_templates=len(templates),
        structures=dict(rand_state='*(0x2151b4)+0x58 = 0x214f20', grid_pointer='0x24e9fc', grid_address='0x%x' % grid_ptr,
                        cache_def_table='0x24e790[66]', cache_elem_table='0x24e898[66]',
                        template_table='*(0x1f849c) = 0x%x, slot = def id + 0x54' % table,
                        job='+0 id, +8 element count, +0xc element array (0x80 each)',
                        element='+0x50 element id (runtime), +0x64 geometry piece chain',
                        piece='+0 next, +4 positions V3-16, +8 UV V2-16, +0xc colours V4-8, +0x10 strips V4-8, '
                              '+0x14 position tags, +0x18 UV tags, +0x1c counts lo16/hi16'),
        snapshots=dict(after_10bc78=s1.sha256, stop=s2.sha256), determinism=determinism, startup_cross_check=cross,
        run_meta=dict(a=meta_a, b=meta_b))
    return draws, effects, summary

def _count(xs):
    out = {}
    for x in xs:
        out[x] = out.get(x, 0) + 1
    return out

def write_fresh(path, data):
    need(not os.path.exists(path), f'refusing to overwrite {path}')
    with open(path, 'x', encoding='utf-8') as f:
        f.write(data)

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('run_a')
    p.add_argument('run_b')
    p.add_argument('--contract', required=True)
    p.add_argument('--functions', required=True)
    p.add_argument('--startup')
    p.add_argument('--out', required=True)
    a = p.parse_args(argv)
    try:
        draws, effects, summary = extract(a.run_a, a.run_b, a.contract, a.functions, a.startup)
    except ExtractError as e:
        print(f'EXTRACT FAILED: {e}', file=sys.stderr)
        return 1
    os.makedirs(a.out, exist_ok=False)
    write_fresh(os.path.join(a.out, 'rng.jsonl'), ''.join(
        json.dumps(dict(i=d['i'], phase=d['phase'], state=d['state'], value=d['value']), separators=(',', ':')) + '\n'
        for d in draws))
    write_fresh(os.path.join(a.out, 'rng-detail.jsonl'), ''.join(
        json.dumps(d, separators=(',', ':')) + '\n' for d in draws))
    write_fresh(os.path.join(a.out, 'effects.json'), json.dumps(effects, separators=(',', ':')) + '\n')
    summary['inputs'] = dict(contract=dict(path=a.contract, sha256=fsha(a.contract)),
                             functions=dict(path=a.functions, sha256=fsha(a.functions)))
    write_fresh(os.path.join(a.out, 'reference.json'), json.dumps(summary, indent=1) + '\n')
    print('draws', len(draws), 'phases', [(b['phase'], b['count'], '0x%08x' % b['state_out']) for b in summary['phases']],
          'templates', len(effects['templates']), 'requests', len(effects['requests']),
          'cache owners', len(effects['cache_owners']))
    return 0

if __name__ == '__main__':
    sys.exit(main())
