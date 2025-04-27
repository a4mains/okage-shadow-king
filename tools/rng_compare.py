#!/usr/bin/env python3
"""Compare native startup RNG draws and effect-preparation products with the RNG original reference.

usage: rng_compare.py ORIGINAL_DIR NATIVE_DIR [--json REPORT] [--noise-triangle-order exact|multiset]

Both directories hold the same schema (ORIGINAL_DIR is written by tools/rng_original_extract.py):

  rng.jsonl     one JSON object per rand() draw, in call order, no blank lines:
                {"i": int (0..n-1, consecutive), "phase": "noise"|"effect-grid"|"effect-template",
                 "state": u32 state AFTER the draw, "value": u32 returned value (state & 0x7fffffff)}
  effects.json  {"grid": [384 u32],
                 "templates": [{"shape": int, "owner": [def_id, elem_id], "order": int (construction order,
                                a permutation of 0..n-1), "draws": int,
                                "vertices": [[12 float32 bit patterns "0x%08x" lowercase:
                                              px,py,pz,pw, cr,cg,cb,ca, u,v,s,t], ...],
                                "position_tags": [u8 per vertex], "uv_tags": [u8 per vertex]}],
                 "cache_owners": [[shape, def_id, elem_id], ...]   every nonzero slot of the 66-entry table,
                 "requests": [{"def": int, "elem": int, "shape": int, "constructed": bool}, ...]}

Comparison: every draw (index, phase, state, value) and the phase boundaries; the grid; templates ordered by
"order" with bitwise vertices and tags; cache owners by shape; template requests in order. The first difference
of each kind is reported. Objects must have exactly the listed keys; duplicate keys, NaN, booleans in integer
fields and non-canonical hex are malformed.

--noise-triangle-order multiset (explicit relaxation, default exact): for shapes 10..17 only, accept equal
multisets of winding-preserving triangles (vertex+tags tuples rotated to a canonical start). The original
0x10e138 output is merged/split into multi-quad strips by the retail strip builder; see
the RNG original-reference contract. All other fields stay exact. The relaxation is recorded in the report.

Exit status: 0 identical, 1 different, 2 malformed input or usage error. Research tool; never product input.
"""
import argparse
import json
import os
import re
import sys

PHASES = ('noise', 'effect-grid', 'effect-template')
HEX32 = re.compile(r'0x[0-9a-f]{8}\Z')
U32 = 0xffffffff

class Malformed(ValueError):
    pass

def _pairs(pairs):
    out = {}
    for k, v in pairs:
        if k in out:
            raise Malformed(f'duplicate key {k!r}')
        out[k] = v
    return out

def _const(name):
    raise Malformed(f'non-finite number {name}')

def loads(text, where):
    try:
        return json.loads(text, object_pairs_hook=_pairs, parse_constant=_const)
    except Malformed as e:
        raise Malformed(f'{where}: {e}')
    except ValueError as e:
        raise Malformed(f'{where}: invalid JSON ({e})')

def is_int(x, lo=0, hi=U32):
    return isinstance(x, int) and not isinstance(x, bool) and lo <= x <= hi

def check_keys(obj, keys, where):
    if not isinstance(obj, dict):
        raise Malformed(f'{where}: expected an object')
    if set(obj) != set(keys):
        missing, extra = sorted(set(keys) - set(obj)), sorted(set(obj) - set(keys))
        raise Malformed(f'{where}: keys differ from schema (missing {missing}, unexpected {extra})')

def read_text(path):
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError as e:
        raise Malformed(f'{path}: {e.strerror}')
    try:
        return data.decode('utf-8')
    except UnicodeDecodeError:
        raise Malformed(f'{path}: not UTF-8')

def load_rng(path):
    text = read_text(path)
    if not text:
        raise Malformed(f'{path}: empty')
    if not text.endswith('\n'):
        raise Malformed(f'{path}: last line must end with a newline')
    draws = []
    for n, line in enumerate(text[:-1].split('\n')):
        where = f'{path}:{n + 1}'
        if not line.strip():
            raise Malformed(f'{where}: blank line')
        d = loads(line, where)
        check_keys(d, ('i', 'phase', 'state', 'value'), where)
        if not is_int(d['i']) or d['i'] != n:
            raise Malformed(f'{where}: "i" must be {n}')
        if d['phase'] not in PHASES:
            raise Malformed(f'{where}: unknown phase {d["phase"]!r}')
        if not is_int(d['state']) or not is_int(d['value']):
            raise Malformed(f'{where}: state/value must be u32 integers')
        draws.append(d)
    return draws

def load_effects(path):
    text = read_text(path)
    if not text.strip():
        raise Malformed(f'{path}: empty')
    e = loads(text, path)
    check_keys(e, ('grid', 'templates', 'cache_owners', 'requests'), path)
    g = e['grid']
    if not isinstance(g, list) or len(g) != 384 or not all(is_int(x) for x in g):
        raise Malformed(f'{path}: grid must be 384 u32 integers')
    if not isinstance(e['templates'], list):
        raise Malformed(f'{path}: templates must be a list')
    orders = []
    for k, t in enumerate(e['templates']):
        where = f'{path}: templates[{k}]'
        check_keys(t, ('shape', 'owner', 'order', 'draws', 'vertices', 'position_tags', 'uv_tags'), where)
        if not is_int(t['shape'], 0, 65) or not is_int(t['order']) or not is_int(t['draws']):
            raise Malformed(f'{where}: shape (0..65)/order/draws must be integers')
        if not (isinstance(t['owner'], list) and len(t['owner']) == 2 and all(is_int(x) for x in t['owner'])):
            raise Malformed(f'{where}: owner must be [def_id, elem_id]')
        v = t['vertices']
        if not isinstance(v, list):
            raise Malformed(f'{where}: vertices must be a list')
        for j, vert in enumerate(v):
            if not (isinstance(vert, list) and len(vert) == 12 and
                    all(isinstance(x, str) and HEX32.match(x) for x in vert)):
                raise Malformed(f'{where}: vertices[{j}] must be 12 lowercase "0x%08x" strings')
        for key in ('position_tags', 'uv_tags'):
            tags = t[key]
            if not (isinstance(tags, list) and len(tags) == len(v) and all(is_int(x, 0, 255) for x in tags)):
                raise Malformed(f'{where}: {key} must hold one u8 per vertex')
        orders.append(t['order'])
    if sorted(orders) != list(range(len(orders))):
        raise Malformed(f'{path}: template "order" values must be a permutation of 0..{len(orders) - 1}')
    if not isinstance(e['cache_owners'], list):
        raise Malformed(f'{path}: cache_owners must be a list')
    seen = set()
    for k, c in enumerate(e['cache_owners']):
        if not (isinstance(c, list) and len(c) == 3 and is_int(c[0], 0, 65) and is_int(c[1]) and is_int(c[2])):
            raise Malformed(f'{path}: cache_owners[{k}] must be [shape 0..65, def_id, elem_id]')
        if c[0] in seen:
            raise Malformed(f'{path}: cache_owners lists shape {c[0]} twice')
        if c[1] == 0 and c[2] == 0:
            raise Malformed(f'{path}: cache_owners[{k}] is a zero slot (list nonzero slots only)')
        seen.add(c[0])
    if not isinstance(e['requests'], list):
        raise Malformed(f'{path}: requests must be a list')
    for k, r in enumerate(e['requests']):
        where = f'{path}: requests[{k}]'
        check_keys(r, ('def', 'elem', 'shape', 'constructed'), where)
        if not (is_int(r['def']) and is_int(r['elem']) and is_int(r['shape']) and isinstance(r['constructed'], bool)):
            raise Malformed(f'{where}: def/elem/shape must be integers and constructed a boolean')
    return e

def load_detail(directory):
    path = os.path.join(directory, 'rng-detail.jsonl')
    if not os.path.isfile(path):
        return None
    out = []
    for line in read_text(path).splitlines():
        out.append(loads(line, path))
    return out

def runs(draws):
    out = []
    for d in draws:
        if out and out[-1]['phase'] == d['phase']:
            out[-1]['last'] = d['i']
            out[-1]['count'] += 1
        else:
            out.append(dict(phase=d['phase'], first=d['i'], last=d['i'], count=1))
    return out

def tri_multiset(t):
    v = t['vertices']
    if len(v) % 3:
        return None
    out = {}
    for i in range(0, len(v), 3):
        tri = [(tuple(v[i + k]), t['position_tags'][i + k], t['uv_tags'][i + k]) for k in range(3)]
        s = min(range(3), key=lambda k: tri[k])
        key = tuple(tri[s:] + tri[:s])
        out[key] = out.get(key, 0) + 1
    return out

def compare(orig_dir, native_dir, noise_order='exact'):
    o_rng = load_rng(os.path.join(orig_dir, 'rng.jsonl'))
    o_fx = load_effects(os.path.join(orig_dir, 'effects.json'))
    n_rng = load_rng(os.path.join(native_dir, 'rng.jsonl'))
    n_fx = load_effects(os.path.join(native_dir, 'effects.json'))
    detail = load_detail(orig_dir)
    report = dict(original=os.path.abspath(orig_dir), native=os.path.abspath(native_dir),
                  noise_triangle_order=noise_order, sections={}, relaxations_applied=[])
    diffs = report['sections']

    # ---- draws ---------------------------------------------------------------------------------------
    d = dict(original_count=len(o_rng), native_count=len(n_rng), differing_draws=0, first=None)
    for a, b in zip(o_rng, n_rng):
        fields = [k for k in ('phase', 'state', 'value') if a[k] != b[k]]
        if fields:
            d['differing_draws'] += 1
            if d['first'] is None:
                d['first'] = dict(i=a['i'], fields=fields, original=a, native=b)
                if detail and a['i'] < len(detail):
                    x = detail[a['i']]
                    d['first']['original_context'] = {k: x.get(k) for k in (
                        'ra', 'call_site', 'caller_function', 'caller_name', 'top_step', 'state_before',
                        'def_id', 'source_elem', 'shape', 'factory') if k in x}
    if len(o_rng) != len(n_rng):
        k = min(len(o_rng), len(n_rng))
        d['length'] = dict(first_unmatched_index=k,
                           missing_in_native=max(0, len(o_rng) - len(n_rng)),
                           extra_in_native=max(0, len(n_rng) - len(o_rng)),
                           first_unmatched=(o_rng[k] if len(o_rng) > k else n_rng[k]))
    inconsistent = [x['i'] for x in n_rng if x['value'] != x['state'] & 0x7fffffff]
    if inconsistent:
        d['native_value_not_state_masked'] = dict(count=len(inconsistent), first=inconsistent[0])
    d['identical'] = d['differing_draws'] == 0 and len(o_rng) == len(n_rng) and not inconsistent
    diffs['draws'] = d
    ob, nb = runs(o_rng), runs(n_rng)
    diffs['phase_boundaries'] = dict(original=ob, native=nb, identical=ob == nb)

    # ---- grid ----------------------------------------------------------------------------------------
    g = [k for k in range(384) if o_fx['grid'][k] != n_fx['grid'][k]]
    diffs['grid'] = dict(identical=not g, differing_cells=len(g),
                         first=None if not g else dict(cell=g[0], face=g[0] // 64, row=g[0] % 64 // 8,
                                                       column=g[0] % 8, original=o_fx['grid'][g[0]],
                                                       native=n_fx['grid'][g[0]]))

    # ---- templates -----------------------------------------------------------------------------------
    ot = sorted(o_fx['templates'], key=lambda t: t['order'])
    nt = sorted(n_fx['templates'], key=lambda t: t['order'])
    tdiff = []
    for k in range(max(len(ot), len(nt))):
        a = ot[k] if k < len(ot) else None
        b = nt[k] if k < len(nt) else None
        if a is None or b is None:
            t = a or b
            tdiff.append(dict(order=k, kind='missing in native' if b is None else 'extra in native',
                              shape=t['shape'], owner=t['owner']))
            continue
        entry = dict(order=k, shape=[a['shape'], b['shape']], owner=[a['owner'], b['owner']], fields=[])
        for key in ('shape', 'owner', 'draws'):
            if a[key] != b[key]:
                entry['fields'].append(key)
        geometry_equal = (a['vertices'] == b['vertices'] and a['position_tags'] == b['position_tags']
                          and a['uv_tags'] == b['uv_tags'])
        if not geometry_equal:
            entry['vertex_counts'] = [len(a['vertices']), len(b['vertices'])]
            first = None
            for j in range(min(len(a['vertices']), len(b['vertices']))):
                if (a['vertices'][j] != b['vertices'][j] or a['position_tags'][j] != b['position_tags'][j]
                        or a['uv_tags'][j] != b['uv_tags'][j]):
                    comps = [c for c in range(12) if a['vertices'][j][c] != b['vertices'][j][c]]
                    first = dict(vertex=j, components=comps,
                                 original=dict(v=a['vertices'][j], ptag=a['position_tags'][j], utag=a['uv_tags'][j]),
                                 native=dict(v=b['vertices'][j], ptag=b['position_tags'][j], utag=b['uv_tags'][j]))
                    break
            if first is None:
                first = dict(vertex=min(len(a['vertices']), len(b['vertices'])), components='length')
            entry['first_vertex_difference'] = first
            # Diagnostic only (still a difference): the original packs positions as int16 1/64 units, so it
            # cannot hold -0.0; a native -0.0 (0x80000000) where the original has +0.0 is flagged here.
            unsign = lambda verts: [[('0x00000000' if x == '0x80000000' else x) for x in vv] for vv in verts]
            entry['differs_only_by_signed_zero'] = (
                unsign(a['vertices']) == unsign(b['vertices']) and a['position_tags'] == b['position_tags']
                and a['uv_tags'] == b['uv_tags'])
            same_tris = tri_multiset(a) is not None and tri_multiset(a) == tri_multiset(b)
            entry['same_winding_preserving_triangle_multiset'] = same_tris
            if (noise_order == 'multiset' and same_tris and a['shape'] == b['shape'] and 10 <= a['shape'] <= 17):
                report['relaxations_applied'].append(dict(order=k, shape=a['shape'],
                                                          rule='noise triangle order (multiset)'))
            else:
                entry['fields'].append('geometry')
        if entry['fields']:
            tdiff.append(entry)
    diffs['templates'] = dict(original_count=len(ot), native_count=len(nt), identical=not tdiff,
                              differences=tdiff, first=tdiff[0] if tdiff else None)

    # ---- cache owners --------------------------------------------------------------------------------
    oc = {c[0]: c[1:] for c in o_fx['cache_owners']}
    nc = {c[0]: c[1:] for c in n_fx['cache_owners']}
    cdiff = [dict(shape=s, original=oc.get(s), native=nc.get(s)) for s in sorted(set(oc) | set(nc))
             if oc.get(s) != nc.get(s)]
    diffs['cache_owners'] = dict(identical=not cdiff, differences=cdiff, first=cdiff[0] if cdiff else None)

    # ---- requests ------------------------------------------------------------------------------------
    orq, nrq = o_fx['requests'], n_fx['requests']
    first = None
    for k in range(max(len(orq), len(nrq))):
        a = orq[k] if k < len(orq) else None
        b = nrq[k] if k < len(nrq) else None
        if a != b:
            first = dict(index=k, original=a, native=b)
            break
    diffs['requests'] = dict(original_count=len(orq), native_count=len(nrq), identical=first is None, first=first)

    # ---- native self-consistency (informational) ------------------------------------------------------
    grid_draws = [x for x in n_rng if x['phase'] == 'effect-grid']
    report['native_consistency'] = dict(
        grid_is_value_mod_7_of_grid_draws=len(grid_draws) == 384 and
        [x['value'] % 7 for x in grid_draws] == n_fx['grid'],
        template_draws_sum=sum(t['draws'] for t in nt),
        effect_template_draws=sum(1 for x in n_rng if x['phase'] == 'effect-template'))
    report['identical'] = all(s['identical'] for s in diffs.values())
    return report

def summary_lines(r):
    s = r['sections']
    yield f"draws: original {s['draws']['original_count']}, native {s['draws']['native_count']}, " \
          f"differing {s['draws']['differing_draws']}" + ('' if s['draws']['identical'] else '  DIFFERENT')
    if s['draws']['first']:
        yield f"  first draw difference: {json.dumps(s['draws']['first'], sort_keys=True)}"
    if 'length' in s['draws']:
        yield f"  length: {json.dumps(s['draws']['length'], sort_keys=True)}"
    if 'native_value_not_state_masked' in s['draws']:
        yield f"  native value != state & 0x7fffffff: {s['draws']['native_value_not_state_masked']}"
    pb = s['phase_boundaries']
    yield 'phase boundaries: ' + ('identical ' if pb['identical'] else 'DIFFERENT ') + \
        ' '.join(f"{x['phase']}[{x['first']}..{x['last']}]" for x in pb['native'])
    if not pb['identical']:
        yield '  original: ' + ' '.join(f"{x['phase']}[{x['first']}..{x['last']}]" for x in pb['original'])
    yield 'grid: ' + ('identical' if s['grid']['identical'] else f"DIFFERENT {json.dumps(s['grid']['first'])}")
    t = s['templates']
    yield f"templates: original {t['original_count']}, native {t['native_count']}" + \
        ('' if t['identical'] else f"  DIFFERENT ({len(t['differences'])}); first {json.dumps(t['first'])}")
    for x in r['relaxations_applied']:
        yield f"  relaxation applied: order {x['order']} shape {x['shape']} ({x['rule']})"
    c = s['cache_owners']
    yield 'cache owners: ' + ('identical' if c['identical'] else f"DIFFERENT ({len(c['differences'])}); first {json.dumps(c['first'])}")
    q = s['requests']
    yield f"requests: original {q['original_count']}, native {q['native_count']}" + \
        ('' if q['identical'] else f"  DIFFERENT; first {json.dumps(q['first'])}")
    yield 'IDENTICAL' if r['identical'] else 'DIFFERENT'

MANIFEST = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs', 'rng-original-manifest.json')
EXPECTED_TOTALS = {'draws': 9504, 'templates': 11, 'requests': 886}

def acceptance_gate(original_dir, native_dir, manifest=MANIFEST):
    """The CLI is an acceptance gate: the original side must be the pinned capture and the two sides distinct."""
    if os.path.realpath(original_dir) == os.path.realpath(native_dir):
        raise Malformed('original and native are the same directory')
    import hashlib
    with open(manifest, encoding='utf-8') as f:
        outputs = json.load(f).get('outputs', {})
    needed = [k for k in outputs if k.startswith('analysis/')]
    if not needed:
        raise Malformed('manifest pins no analysis outputs')
    for key in needed:
        path = os.path.join(original_dir, key[len('analysis/'):])
        if not os.path.isfile(path):
            raise Malformed(f'{path}: pinned original file missing')
        with open(path, 'rb') as f:
            data = f.read()
        if hashlib.sha256(data).hexdigest() != outputs[key]['sha256'] or len(data) != outputs[key]['bytes']:
            raise Malformed(f'{path}: does not match the pinned capture in {os.path.basename(manifest)}')

def check_totals(report):
    s = report['sections']
    seen = {'draws': (s['draws']['original_count'], s['draws']['native_count']),
            'templates': (s['templates']['original_count'], s['templates']['native_count']),
            'requests': (s['requests']['original_count'], s['requests']['native_count'])}
    for name, (o, n) in seen.items():
        if o != EXPECTED_TOTALS[name] or n != EXPECTED_TOTALS[name]:
            raise Malformed(f'{name}: original {o}, native {n}; the startup prefix has {EXPECTED_TOTALS[name]}')

def main(argv=None, _acceptance=True):
    """_acceptance=False is for synthetic unit fixtures only; the command line always gates."""
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('original_dir')
    p.add_argument('native_dir')
    p.add_argument('--json', help='write the full report here (must not exist)')
    p.add_argument('--noise-triangle-order', choices=('exact', 'multiset'), default='exact')
    try:
        a = p.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0
    try:
        for d in (a.original_dir, a.native_dir):
            if not os.path.isdir(d):
                raise Malformed(f'{d}: not a directory')
        if _acceptance:
            acceptance_gate(a.original_dir, a.native_dir)
        report = compare(a.original_dir, a.native_dir, a.noise_triangle_order)
        if _acceptance:
            check_totals(report)
    except Malformed as e:
        print(f'MALFORMED: {e}', file=sys.stderr)
        return 2
    for line in summary_lines(report):
        print(line)
    if a.json:
        if os.path.exists(a.json):
            print(f'MALFORMED: refusing to overwrite {a.json}', file=sys.stderr)
            return 2
        with open(a.json, 'x', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
            f.write('\n')
    return 0 if report['identical'] else 1

if __name__ == '__main__':
    sys.exit(main())
