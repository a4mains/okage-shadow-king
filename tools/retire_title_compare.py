#!/usr/bin/env python3
"""Compare a native Session title-retirement report with the unchanged-retail Retire original reference.

usage: retire_title_compare.py ORIGINAL_ANALYSIS_DIR NATIVE_DIR [--manifest PATH] [--json OUT]

ORIGINAL_ANALYSIS_DIR is the analysis/ directory written by tools/retire_title_original_extract.py (both routes).
Every original file used must equal its sha256/size pin in docs/retire-title-original-manifest.json (or --manifest),
or the run is refused (exit 2).

NATIVE_DIR:
  route.json   {"route": "restore"|"newgame", "elf_sha256": "<pinned SCUS_971.29 sha256>",
                "checkpoints": {"first-update": F, "stable-menu": F, "removal-request": F,
                                "resource-retirement": F, "successor-entry": F}}
               exactly these keys; F = the native frame index (integer >= 0), strictly increasing in that order.
               Frames are reported, never compared with the original's observer-dependent vblanks.
  <checkpoint>/ for each of the five checkpoints:
    lifecycle.jsonl   the ordered Session event stream FROM COLD BOOT up to the checkpoint (a checkpoint at a function
                      entry precedes every event of that call). Title kinds (alloc upload ref free handle retire bind name
                      reserve material phase) with the Title keys, plus
                        {"kind":"effect","op":"create","shape":s,"slot":n}            0x101960 return
                        {"kind":"effect","op":"bind","slot":n,"element":e,"handle":h}  0x10c188 return
                        {"kind":"effect","op":"remove-request","slot":n}               0x103640 entry (n<0: bulk, as passed)
                        {"kind":"effect","op":"reclaim","slot":n}                      0x102218 job free (0x103bf8 of the job)
                        {"kind":"mode","op":"request","mode":m,"selector":s,"arg":a}  0x124930 entry
                        {"kind":"mode","op":"resource-retire","mode":8,"selector":s}  0x101058 entry (resource child dtor)
                        {"kind":"mode","op":"destroy","mode":8,"selector":s}          0x12c8d8 entry (mode-8 dtor)
                        {"kind":"mode","op":"wrapper-free","node":"resource"|"mode8","mode":8,"selector":s}
                                                                                      0x1bdaf0 from the scheduler unlink
                      Keys must match exactly; phase rows are informational and never compared.
    state.json        the Title state.json schema; coverage must be
                      {"lifecycle":"retire-title","checkpoint":"<name>","memory":"host-uploads-known-draw-writes-unknown"};
                      materials may be [] (model retired).
    effects.json      {"effect106": null | {"slot":n,"shape":106|null,"state":"active"|"removal-requested"|
                       "reclaim-pending"|"reclaimed","element_handles":[h,...]}}; the RNG keys grid, templates,
                      cache_owners, requests are accepted and ignored here (tools/rng_compare.py).
    rng.jsonl         every Session rand() draw before the checkpoint: {"i","phase","state","value"}, i = 0..n-1,
                      phase in noise|effect-grid|effect-template|session ("session" = after 0x100618).
    vram.bytes, vram.known   4 MiB each (known bit0 = low nibble, bit1 = high nibble).
    vram.invalidated  optional, 4 MiB, same bit layout: nibbles the native report marks unknown because a draw (or an
                      unmodelled writer) may have overwritten them. Must not overlap vram.known.

Comparison at each checkpoint: the ordered non-phase events exactly; every state field exactly; effect106 exactly;
the draws exactly; and GS local memory per nibble:
  * original last writer = upload:  native known with the same value; native unknown AND invalidated = coverage gap
    (reported, not a failure); native unknown and not invalidated = missing_knowledge (failure);
  * original last writer = draw/local-to-local: native must be unknown (stale_known);
  * never written in the original: native must be unknown (invented_knowledge).
Each checkpoint's lifecycle and draws must extend the previous checkpoint's (else stream_inconsistent).
Exit status: 0 identical (coverage gaps allowed), 1 differences, 2 malformed/unpinned input, mismatched identity
or self-comparison. Research tool; never product input.
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

MANIFEST = Path(__file__).resolve().parents[1] / 'docs/retire-title-original-manifest.json'
VRAM = 1 << 22
ROUTES = ('restore', 'newgame')
CHECKPOINTS = ('first-update', 'stable-menu', 'removal-request', 'resource-retirement', 'successor-entry')
MEMORY = 'host-uploads-known-draw-writes-unknown'
RNG_PHASES = ('noise', 'effect-grid', 'effect-template', 'session')
EFFECT_STATES = ('active', 'removal-requested', 'reclaim-pending', 'reclaimed')
EVENT_KEYS = {
    'alloc': {'alloc': int, 'class': int, 'psm': int, 'w': int, 'h': int, 'tbp': int},
    'upload': {'alloc': int},
    'ref': {'alloc': int, 'delta': int, 'refs': int},
    'free': {'alloc': int},
    'handle': {'handle': int, 'record': int, 'alias': bool},
    'retire': {'handle': int},
    'bind': {'handle': int, 'level': int, 'alloc': int},
    'name': {'handle': int, 'name': (str, type(None))},
    'reserve': {'count': int, 'first': int, 'class': int},
    'material': {'model': str, 'slot': int, 'handle': int},
    'phase': {'name': str},
}
OP_KEYS = {
    'effect': {
        'create': {'shape': int, 'slot': int},
        'bind': {'slot': int, 'element': int, 'handle': int},
        'remove-request': {'slot': int},
        'reclaim': {'slot': int},
    },
    'mode': {
        'request': {'mode': int, 'selector': int, 'arg': int},
        'resource-retire': {'mode': int, 'selector': int},
        'destroy': {'mode': int, 'selector': int},
        'wrapper-free': {'node': ('resource', 'mode8'), 'mode': int, 'selector': int},
    },
}
STATE_KEYS = ('allocations', 'handles', 'records', 'hash', 'reserved', 'free_records', 'free_extras', 'available',
              'cursors', 'free_queue_ids', 'occupancy_hex', 'coverage', 'image_list', 'materials')
IGNORED_STATE_KEYS = ('later_history', 'rand_state_after_noise', 'rng_draws', 'checkpoints', 'layers', 'filter_cluts')
ALLOC_KEYS = ('id', 'serial', 'class', 'psm', 'width', 'height', 'tbp', 'tbw', 'references', 'descriptor')
IGNORED_EFFECT_KEYS = ('grid', 'templates', 'cache_owners', 'requests')
ORIGINAL_FILES = ('events.jsonl', 'state.json', 'effects.json', 'rng.jsonl', 'vram-written-mask.bin',
                  'vram-written-values.bin', 'vram-invalidated-mask.bin')
NATIVE_FILES = ('lifecycle.jsonl', 'state.json', 'effects.json', 'rng.jsonl', 'vram.bytes', 'vram.known', 'vram.invalidated')

class Malformed(Exception):
    pass

def _no_dup(pairs):
    out = {}
    for k, v in pairs:
        if k in out:
            raise ValueError(f'duplicate key {k}')
        out[k] = v
    return out

def _bad_constant(c):
    raise ValueError('non-finite number ' + c)

def parse_json(text, where):
    try:
        return json.loads(text, object_pairs_hook=_no_dup, parse_constant=_bad_constant)
    except ValueError as exc:
        raise Malformed(f'{where}: {exc}')

def read_text(path):
    try:
        return Path(path).read_text()
    except (OSError, UnicodeDecodeError) as exc:
        raise Malformed(str(exc))

def is_int(v):
    return type(v) is int

def typed(v, t):
    if t is int:
        return is_int(v)
    if t is bool:
        return type(v) is bool
    if isinstance(t, tuple) and all(isinstance(x, str) for x in t):
        return type(v) is str and v in t
    if isinstance(t, tuple):
        return any(typed(v, x) for x in t)
    return type(v) is t

def canon(e):
    return json.dumps(e, sort_keys=True, separators=(',', ':'))

def jsonl_rows(path):
    text = read_text(path)
    if text and not text.endswith('\n'):
        raise Malformed(f'{path}: last line is not newline-terminated (truncated?)')
    rows = []
    for n, line in enumerate(text.split('\n')[:-1] if text else []):
        if not line.strip():
            raise Malformed(f'{path}:{n + 1}: empty line')
        rows.append((n + 1, parse_json(line, f'{path}:{n + 1}')))
    return rows

# ---- lifecycle -------------------------------------------------------------------------------------
def load_lifecycle(path):
    rows = []
    for n, e in jsonl_rows(path):
        if not isinstance(e, dict) or e.get('kind') not in set(EVENT_KEYS) | set(OP_KEYS):
            raise Malformed(f'{path}:{n}: unknown event kind {e.get("kind") if isinstance(e, dict) else e!r}')
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
            if not typed(e[k], t):
                raise Malformed(f'{path}:{n}: {e["kind"]}.{k} = {e[k]!r} has the wrong type/value')
        rows.append(e)
    if not any(r['kind'] != 'phase' for r in rows):
        raise Malformed(f'{path}: no lifecycle events')
    return rows

def kind_key(e):
    return e['kind'] + (':' + e['op'] if 'op' in e else '')

def compare_events(orig, nat, out):
    a = [e for e in orig if e['kind'] != 'phase']
    b = [e for e in nat if e['kind'] != 'phase']
    kinds = sorted(set(kind_key(e) for e in a + b))
    ca = {k: sum(1 for e in a if kind_key(e) == k) for k in kinds}
    cb = {k: sum(1 for e in b if kind_key(e) == k) for k in kinds}
    print('    event counts original: ' + ', '.join(f'{k}={ca[k]}' for k in kinds), file=out)
    if ca != cb:
        print('    event counts native:   ' + ', '.join(f'{k}={cb[k]}' for k in kinds), file=out)
    first = None
    for i in range(max(len(a), len(b))):
        if i >= len(a) or i >= len(b) or canon(a[i]) != canon(b[i]):
            first = i
            break
    if first is None:
        print(f'    events: identical ({len(a)} in order)', file=out)
        return 0, None
    print(f'    events: DIFFERENT at event {first} (of {len(a)} original / {len(b)} native)', file=out)
    for i in range(max(0, first - 2), min(max(len(a), len(b)), first + 3)):
        x = canon(a[i]) if i < len(a) else '<absent>'
        y = canon(b[i]) if i < len(b) else '<absent>'
        print(f'      [{i}] {"==" if x == y else "!="} original {x} | native {y}', file=out)
    return 1, first

# ---- state -----------------------------------------------------------------------------------------
def _ints(v, where, n=None):
    if not isinstance(v, list) or any(not is_int(x) for x in v) or (n is not None and len(v) != n):
        raise Malformed(f'{where}: not a list of {"%d " % n if n is not None else ""}integers')
    return v

def _pair(v, where, nullable=True):
    if v is None and nullable:
        return v
    _ints(v, where, 2)
    return v

def _ascending(vals, where):
    if any(b <= a for a, b in zip(vals, vals[1:])):
        raise Malformed(f'{where}: not strictly ascending')

def load_state(path, cp):
    s = parse_json(read_text(path), str(path))
    if not isinstance(s, dict):
        raise Malformed(f'{path}: not an object')
    missing = set(STATE_KEYS) - set(s)
    unknown = set(s) - set(STATE_KEYS) - set(IGNORED_STATE_KEYS)
    if missing or unknown:
        raise Malformed(f'{path}: missing keys {sorted(missing)}, unknown keys {sorted(unknown)}')
    cov = {'lifecycle': 'retire-title', 'checkpoint': cp, 'memory': MEMORY}
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

def compare_state(orig, nat, out):
    diffs = 0
    same = []
    for key in STATE_KEYS:
        if key == 'coverage':
            continue
        a, b = orig[key], nat[key]
        if canon(a) == canon(b):
            same.append(key)
            continue
        diffs += 1
        detail = f'original {a!r} | native {b!r}'
        if isinstance(a, list) and isinstance(b, list):
            for i in range(max(len(a), len(b))):
                x = a[i] if i < len(a) else '<absent>'
                y = b[i] if i < len(b) else '<absent>'
                if x != y:
                    detail = f'first difference at [{i}] (lengths {len(a)}/{len(b)}): original {x!r} | native {y!r}'
                    break
        elif isinstance(a, str):
            i = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
            detail = f'first difference at character {i}'
        print(f'    state.{key}: DIFFERENT {detail}'[:600], file=out)
    print(f'    state: {len(same)} of {len(STATE_KEYS) - 1} fields identical '
          f'(allocations {len(orig["allocations"])}, handles {len(orig["handles"])}, records {len(orig["records"])}, '
          f'image_list {orig["image_list"]}, materials {len(orig["materials"])})', file=out)
    return diffs

# ---- effects / rng ---------------------------------------------------------------------------------
def load_effects(path):
    e = parse_json(read_text(path), str(path))
    if not isinstance(e, dict) or 'effect106' not in e or set(e) - {'effect106'} - set(IGNORED_EFFECT_KEYS):
        raise Malformed(f'{path}: must hold "effect106" (and only the ignored RNG keys {IGNORED_EFFECT_KEYS})')
    x = e['effect106']
    if x is not None:
        if not isinstance(x, dict) or set(x) != {'slot', 'shape', 'state', 'element_handles'} or not is_int(x['slot']) \
                or not (x['shape'] is None or is_int(x['shape'])) or x['state'] not in EFFECT_STATES:
            raise Malformed(f'{path}: effect106 {x!r} has the wrong keys/types')
        _ints(x['element_handles'], 'effect106.element_handles')
    return x

def load_rng(path):
    rows = []
    for n, d in jsonl_rows(path):
        if not isinstance(d, dict) or set(d) != {'i', 'phase', 'state', 'value'}:
            raise Malformed(f'{path}:{n}: rng row keys')
        if not (is_int(d['i']) and is_int(d['state']) and is_int(d['value'])) or d['phase'] not in RNG_PHASES \
                or not 0 <= d['state'] <= 0xffffffff or not 0 <= d['value'] <= 0xffffffff or d['i'] != len(rows):
            raise Malformed(f'{path}:{n}: rng row {d!r}')
        rows.append(d)
    return rows

def compare_rng(orig, nat, out):
    for i in range(max(len(orig), len(nat))):
        if i >= len(orig) or i >= len(nat) or orig[i] != nat[i]:
            print(f'    rng: DIFFERENT at draw {i} (of {len(orig)} original / {len(nat)} native): original '
                  f'{orig[i] if i < len(orig) else "<absent>"} | native {nat[i] if i < len(nat) else "<absent>"}', file=out)
            return 1, i
    after = sum(1 for d in orig if d['phase'] == 'session')
    print(f'    rng: identical ({len(orig)} draws, {after} after 0x100618)', file=out)
    return 0, None

_EXPAND = bytes(((0x0f if b & 1 else 0) | (0xf0 if b & 2 else 0)) if b < 4 else 0 for b in range(256))
_POP = bytes(((b & 1) + ((b >> 1) & 1)) if b < 4 else 0 for b in range(256))
_NZ = bytes(((b & 0x0f) != 0) | (((b & 0xf0) != 0) << 1) for b in range(256))

def _op(a, b, f):
    return f(int.from_bytes(a, 'little'), int.from_bytes(b, 'little')).to_bytes(VRAM, 'little')

def _and(a, b):
    return _op(a, b, lambda x, y: x & y)

def _andnot(a, b):
    return _op(a, b, lambda x, y: x & ~y)

def _or(a, b):
    return _op(a, b, lambda x, y: x | y)

def _count(m):
    return sum(m.translate(_POP))

def _first(m):
    s = m.lstrip(b'\0')
    if not s:
        return None
    i = VRAM - len(s)
    return f'0x{i:06x}.{"lo" if m[i] & 1 else "hi"}'

def read_exact(path, n):
    try:
        data = Path(path).read_bytes()
    except OSError as exc:
        raise Malformed(str(exc))
    if len(data) != n:
        raise Malformed(f'{path}: {len(data)} bytes, expected {n}')
    return data

def _two_bit(data, where):
    if data.translate(None, b'\0\1\2\3'):
        raise Malformed(f'{where} uses bits other than bit0/bit1')
    return data

def compare_vram(orig_dir, nat_dir, out):
    M = _two_bit(read_exact(Path(orig_dir) / 'vram-written-mask.bin', VRAM), 'original written mask')
    V = read_exact(Path(orig_dir) / 'vram-written-values.bin', VRAM)
    T = _two_bit(read_exact(Path(orig_dir) / 'vram-invalidated-mask.bin', VRAM), 'original invalidated mask')
    K = _two_bit(read_exact(Path(nat_dir) / 'vram.known', VRAM), 'vram.known')
    D = read_exact(Path(nat_dir) / 'vram.bytes', VRAM)
    ip = Path(nat_dir) / 'vram.invalidated'
    I = _two_bit(read_exact(ip, VRAM), 'vram.invalidated') if ip.exists() else bytes(VRAM)
    if _count(_and(M, T)):
        raise Malformed('original masks overlap')
    if _count(_and(K, I)):
        raise Malformed(f'vram.known and vram.invalidated overlap (first {_first(_and(K, I))})')
    mk = _and(M, K)
    diff = _op(_op(V, D, lambda x, y: x ^ y), mk.translate(_EXPAND), lambda x, y: x & y).translate(_NZ)
    missing_all = _andnot(M, K)
    gap = _and(missing_all, I)
    missing = _andnot(missing_all, I)
    stale = _and(T, K)
    invented = _andnot(_andnot(K, M), T)
    counts = {'required_known': _count(M), 'required_unknown': _count(T), 'native_known': _count(K),
              'native_invalidated': _count(I), 'coverage_gap_invalidated': _count(gap),
              'missing_knowledge': _count(missing), 'value_mismatch': _count(diff), 'stale_known': _count(stale),
              'invented_knowledge': _count(invented)}
    first = {k: f for k, f in (('missing_knowledge', _first(missing)), ('value_mismatch', _first(diff)),
                               ('stale_known', _first(stale)), ('invented_knowledge', _first(invented)),
                               ('coverage_gap_invalidated', _first(gap))) if f}
    bad = sum(counts[k] for k in ('missing_knowledge', 'value_mismatch', 'stale_known', 'invented_knowledge'))
    print('    vram: ' + ', '.join(f'{k}={v}' for k, v in counts.items())
          + (('; first ' + ', '.join(f'{k} at {v}' for k, v in first.items())) if first else ''), file=out)
    return int(bool(bad)), counts, first

def load_manifest(path):
    try:
        m = parse_json(Path(path).read_text(), str(path))
    except OSError as exc:
        raise Malformed(f'manifest: {exc}')
    if not isinstance(m, dict) or not isinstance(m.get('outputs'), dict) or not isinstance(m.get('result_digest'), dict):
        raise Malformed('original manifest lacks outputs/result_digest')
    try:
        elf = m['inputs']['elf']['sha256']
    except (KeyError, TypeError):
        raise Malformed('manifest lacks inputs.elf.sha256')
    return m, elf

def verify_original(orig_dir, route, manifest):
    outputs = manifest['outputs']
    for cp in CHECKPOINTS:
        for name in ORIGINAL_FILES:
            rel = f'analysis/{route}/{cp}/{name}'
            pin = outputs.get(rel)
            if not isinstance(pin, dict):
                raise Malformed(f'manifest does not pin {rel}')
            try:
                data = (Path(orig_dir) / route / cp / name).read_bytes()
            except OSError as exc:
                raise Malformed(str(exc))
            if hashlib.sha256(data).hexdigest() != pin.get('sha256') or len(data) != pin.get('bytes'):
                raise Malformed(f'{rel} does not match the pinned original capture in the manifest')

def load_route(nat_dir, elf):
    r = parse_json(read_text(Path(nat_dir) / 'route.json'), 'route.json')
    if not isinstance(r, dict) or set(r) != {'route', 'elf_sha256', 'checkpoints'}:
        raise Malformed('route.json must hold exactly route, elf_sha256, checkpoints')
    if r['route'] not in ROUTES:
        raise Malformed(f'route.json: unknown route {r["route"]!r}')
    if r['elf_sha256'] != elf:
        raise Malformed('route.json: ELF identity differs from the pinned SCUS_971.29 (addresses would not apply)')
    c = r['checkpoints']
    if not isinstance(c, dict) or set(c) != set(CHECKPOINTS) or any(not is_int(v) or v < 0 for v in c.values()):
        raise Malformed(f'route.json: checkpoints must map exactly {CHECKPOINTS} to non-negative integer frames')
    frames = [c[k] for k in CHECKPOINTS]
    if any(b <= a for a, b in zip(frames, frames[1:])):
        raise Malformed(f'route.json: checkpoint frames {frames} are not strictly increasing (duplicated checkpoint?)')
    return r

def refuse_self_compare(orig_dir, nat_dir):
    o, n = Path(orig_dir).resolve(), Path(nat_dir).resolve()
    if o == n or o in n.parents or n in o.parents:
        raise Malformed('refusing to compare the original analysis with itself (same or nested directory)')
    originals = [p for p in o.rglob('*') if p.is_file()]
    for cp in CHECKPOINTS:
        for nn in NATIVE_FILES:
            p = n / cp / nn
            if not p.exists():
                continue
            for q in originals:
                if os.path.samefile(p, q):
                    raise Malformed(f'refusing self-comparison: native {cp}/{nn} is the original {q.relative_to(o)}')

def run(orig_dir, nat_dir, out=None, manifest=MANIFEST):
    out = out or sys.stdout
    refuse_self_compare(orig_dir, nat_dir)
    m, elf = load_manifest(manifest)
    route = load_route(nat_dir, elf)
    rname = route['route']
    verify_original(orig_dir, rname, m)
    digest = m['result_digest'].get('routes', {}).get(rname)
    if not isinstance(digest, dict):
        raise Malformed(f'manifest result digest lacks route {rname}')
    loaded = {}
    for cp in CHECKPOINTS:
        od, nd = Path(orig_dir) / rname / cp, Path(nat_dir) / cp
        if not nd.is_dir():
            raise Malformed(f'native checkpoint directory {cp} missing')
        loaded[cp] = {
            'o_ev': load_lifecycle(od / 'events.jsonl'), 'n_ev': load_lifecycle(nd / 'lifecycle.jsonl'),
            'o_st': load_state(od / 'state.json', cp), 'n_st': load_state(nd / 'state.json', cp),
            'o_fx': load_effects(od / 'effects.json'), 'n_fx': load_effects(nd / 'effects.json'),
            'o_rng': load_rng(od / 'rng.jsonl'), 'n_rng': load_rng(nd / 'rng.jsonl'),
        }
        d = digest.get(cp)
        if not isinstance(d, dict) or d.get('resource_events') != sum(1 for e in loaded[cp]['o_ev'] if e['kind'] != 'phase') \
                or d.get('rng_draws') != len(loaded[cp]['o_rng']):
            raise Malformed(f'manifest result digest does not match the original {rname}/{cp}')
    print(f'Retire title retirement, route {rname}, original vs native:', file=out)
    result = {'route': rname, 'native_frames': route['checkpoints'], 'checkpoints': {}}
    failed = False
    prev = None
    for cp in CHECKPOINTS:
        L = loaded[cp]
        print(f'  [{cp}] native frame {route["checkpoints"][cp]}', file=out)
        e, first = compare_events(L['o_ev'], L['n_ev'], out)
        s = compare_state(L['o_st'], L['n_st'], out)
        fx = int(canon(L['o_fx']) != canon(L['n_fx']))
        print(f'    effect106: {"identical" if not fx else "DIFFERENT"} original {L["o_fx"]} | native {L["n_fx"]}'
              if fx else f'    effect106: identical {L["o_fx"]}', file=out)
        r, rfirst = compare_rng(L['o_rng'], L['n_rng'], out)
        v, vcounts, vfirst = compare_vram(Path(orig_dir) / rname / cp, Path(nat_dir) / cp, out)
        d = digest[cp]
        if vcounts['required_known'] != d.get('vram_known_nibbles') or vcounts['required_unknown'] != d.get('vram_unknown_nibbles'):
            raise Malformed(f'original VRAM masks of {cp} do not carry the manifest nibble counts')
        inconsistent = []
        if prev is not None:
            pe = [x for x in loaded[prev]['n_ev'] if x['kind'] != 'phase']
            ne = [x for x in L['n_ev'] if x['kind'] != 'phase']
            if [canon(x) for x in ne[:len(pe)]] != [canon(x) for x in pe]:
                inconsistent.append('lifecycle')
            if L['n_rng'][:len(loaded[prev]['n_rng'])] != loaded[prev]['n_rng']:
                inconsistent.append('rng')
        if inconsistent:
            print(f'    stream_inconsistent: native {inconsistent} do not extend checkpoint {prev}', file=out)
        ok = not (e or s or fx or r or v or inconsistent)
        failed |= not ok
        result['checkpoints'][cp] = {'events_identical': not e, 'first_event_difference': first, 'state_differences': s,
                                     'effect106_identical': not fx, 'rng_identical': not r, 'first_rng_difference': rfirst,
                                     'vram': vcounts, 'vram_first': vfirst, 'stream_inconsistent': inconsistent,
                                     'identical': ok}
        prev = cp
    gaps = sum(c['vram']['coverage_gap_invalidated'] for c in result['checkpoints'].values())
    print(f'  not compared: phase rows (informational), native frame numbers vs original vblanks (observer-dependent); '
          f'coverage gaps (original-written nibbles the native report marks draw-invalidated): {gaps}', file=out)
    result['identical'] = not failed
    print('RESULT: ' + ('IDENTICAL' if not failed else 'DIFFERENT'), file=out)
    return (0 if not failed else 1), result

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('original')
    p.add_argument('native')
    p.add_argument('--manifest', default=str(MANIFEST))
    p.add_argument('--json')
    a = p.parse_args(argv)
    try:
        code, result = run(a.original, a.native, manifest=a.manifest)
    except Malformed as exc:
        print('MALFORMED: ' + str(exc), file=sys.stderr)
        return 2
    if a.json:
        with open(a.json, 'x') as f:
            json.dump(result, f, indent=1)
            f.write('\n')
    return code

if __name__ == '__main__':
    sys.exit(main())
