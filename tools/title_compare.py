#!/usr/bin/env python3
"""Compare a native cold-Session report with the unchanged-retail Title logo/title original reference.

usage: title_compare.py ORIGINAL_ANALYSIS_DIR NATIVE_REPORT_DIR [--manifest PATH] [--json OUT]

ORIGINAL_ANALYSIS_DIR is the analysis/ directory written by tools/title_original_extract.py
(the title original-reference contract). Its files must equal the sha256/size pins of
docs/title-original-manifest.json (or --manifest), or the run is refused (exit 2).

NATIVE_REPORT_DIR must hold:
  timeline.jsonl   one JSON object per ordered event:
      {"kind":"alloc","alloc":id,"class":c,"psm":p,"w":w,"h":h,"tbp":t}   0x1cf008 (failure: alloc -1, tbp -1)
      {"kind":"upload","alloc":id}                                     0x1c9bb8 host-to-local upload
      {"kind":"ref","alloc":id,"delta":d,"refs":after}                 0x1cee50 on a non-null descriptor
      {"kind":"free","alloc":id}                                       0x1cf728 that actually freed
      {"kind":"handle","handle":h,"record":r,"alias":bool}             0x1cc880 (alias false) / 0x1ccd48 (true)
      {"kind":"retire","handle":h}                                     0x1cca30 on a mapped handle
      {"kind":"bind","handle":h,"level":l,"alloc":id or -1}            0x1cd298 on a mapped handle
      {"kind":"name","handle":h,"name":s}                              0x1cc4e0 on a mapped handle (s as passed)
      {"kind":"reserve","count":n,"first":f,"class":c}                 0x1cd5c8
      {"kind":"material","model":"NIGHT_V/a.xmd","slot":i,"handle":h}  0x182e28 table entries
      {"kind":"phase","name":s}                                        informational, never compared
    Order: entry of upload/retire/bind/name; return of alloc/ref/free/handle/reserve/material.
  state.json       allocations [{id,serial,class,psm,width,height,tbp,tbw,references,descriptor}] (live, ascending id),
                   handles [{handle,record,image,palettes}] (image/palette = [id,serial] or null), records
                   [{record,name,hash_slot,handles}], hash [[slot,value]], reserved (class-0 queue), free_records,
                   free_extras, available, cursors (64), free_queue_ids, occupancy_hex (8192 bytes), coverage
                   {"lifecycle":"title-constructor-return","memory":"host-uploads-known-draw-writes-unknown"},
                   image_list (0x337508 handles in order), materials [[slot,handle]]. Informational keys
                   later_history, rand_state_after_noise, rng_draws, checkpoints, layers, filter_cluts are ignored.
  vram.bytes, vram.known   4 MiB each; known bit0 = low nibble, bit1 = high nibble (no other bits).

Compared: the ordered resource events exactly (phase markers removed on both sides; no other normalisation
on this side: the original was normalised to source identities by the extractor), every state field
exactly, and GS local memory per nibble:
  * written last by a host-to-local upload in the original (not touched afterwards by a draw/local-to-local
    writer per the GS ledger): native must be KNOWN with the same value;
  * last touched by a draw/local-to-local writer: native must be UNKNOWN (stale-known is a failure);
  * never written in the original: native must be UNKNOWN (invented knowledge is a failure; nothing the
    original did not write is ever required).
Exit status: 0 identical, 1 differences, 2 malformed/unpinned input or self-comparison.
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

MANIFEST = Path(__file__).resolve().parents[1] / 'docs/title-original-manifest.json'
VRAM = 1 << 22
COVERAGE = {'lifecycle': 'title-constructor-return', 'memory': 'host-uploads-known-draw-writes-unknown'}
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
STATE_KEYS = ('allocations', 'handles', 'records', 'hash', 'reserved', 'free_records', 'free_extras', 'available',
              'cursors', 'free_queue_ids', 'occupancy_hex', 'coverage', 'image_list', 'materials')
IGNORED_STATE_KEYS = ('later_history', 'rand_state_after_noise', 'rng_draws', 'checkpoints', 'layers', 'filter_cluts')
ALLOC_KEYS = ('id', 'serial', 'class', 'psm', 'width', 'height', 'tbp', 'tbw', 'references', 'descriptor')
ORIGINAL_FILES = ('events.jsonl', 'final-state.json', 'vram-written-mask.bin', 'vram-written-values.bin',
                  'vram-invalidated-mask.bin', 'gs-writers.json')

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

def is_int(v):
    return type(v) is int

def typed(v, t):
    if t is int:
        return is_int(v)
    if t is bool:
        return type(v) is bool
    if isinstance(t, tuple):
        return any(typed(v, x) for x in t)
    return type(v) is t

# ---- timeline -------------------------------------------------------------------------------------
def load_timeline(path):
    try:
        text = Path(path).read_text()
    except OSError as exc:
        raise Malformed(str(exc))
    if text and not text.endswith('\n'):
        raise Malformed(f'{path}: last line is not newline-terminated (truncated?)')
    rows = []
    for n, line in enumerate(text.split('\n')[:-1] if text else []):
        if not line.strip():
            raise Malformed(f'{path}:{n + 1}: empty line')
        e = parse_json(line, f'{path}:{n + 1}')
        if not isinstance(e, dict) or e.get('kind') not in EVENT_KEYS:
            raise Malformed(f'{path}:{n + 1}: unknown event kind {e.get("kind") if isinstance(e, dict) else e!r}')
        spec = EVENT_KEYS[e['kind']]
        if set(e) != set(spec) | {'kind'}:
            raise Malformed(f'{path}:{n + 1}: {e["kind"]} keys {sorted(e)} != {sorted(set(spec) | {"kind"})}')
        for k, t in spec.items():
            if not typed(e[k], t):
                raise Malformed(f'{path}:{n + 1}: {e["kind"]}.{k} has type {type(e[k]).__name__}')
        rows.append(e)
    if not any(r['kind'] != 'phase' for r in rows):
        raise Malformed(f'{path}: no resource events')
    return rows

def canon(e):
    return json.dumps(e, sort_keys=True, separators=(',', ':'))

def compare_timeline(orig, nat, out):
    a = [e for e in orig if e['kind'] != 'phase']
    b = [e for e in nat if e['kind'] != 'phase']
    kinds = sorted(set(k for k in EVENT_KEYS if k != 'phase'))
    ca = {k: sum(1 for e in a if e['kind'] == k) for k in kinds}
    cb = {k: sum(1 for e in b if e['kind'] == k) for k in kinds}
    print('  event counts original: ' + ', '.join(f'{k}={ca[k]}' for k in kinds), file=out)
    print('  event counts native:   ' + ', '.join(f'{k}={cb[k]}' for k in kinds), file=out)
    first = None
    for i in range(max(len(a), len(b))):
        x = canon(a[i]) if i < len(a) else '<absent>'
        y = canon(b[i]) if i < len(b) else '<absent>'
        if x != y:
            first = i
            break
    if first is None:
        print(f'  timeline: identical ({len(a)} resource events in order)', file=out)
        return 0, None
    print(f'  timeline: DIFFERENT at resource event {first} (of {len(a)} original / {len(b)} native)', file=out)
    for i in range(max(0, first - 2), min(max(len(a), len(b)), first + 3)):
        x = canon(a[i]) if i < len(a) else '<absent>'
        y = canon(b[i]) if i < len(b) else '<absent>'
        print(f'    [{i}] {"==" if x == y else "!="} original {x} | native {y}', file=out)
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

def load_state(path):
    try:
        s = parse_json(Path(path).read_text(), str(path))
    except OSError as exc:
        raise Malformed(str(exc))
    if not isinstance(s, dict):
        raise Malformed(f'{path}: not an object')
    missing = set(STATE_KEYS) - set(s)
    unknown = set(s) - set(STATE_KEYS) - set(IGNORED_STATE_KEYS)
    if missing or unknown:
        raise Malformed(f'{path}: missing keys {sorted(missing)}, unknown keys {sorted(unknown)}')
    if s['coverage'] != COVERAGE:
        raise Malformed(f'{path}: coverage {s["coverage"]!r} != {COVERAGE!r}')
    for a in s['allocations'] if isinstance(s['allocations'], list) else [None]:
        if not isinstance(a, dict) or set(a) != set(ALLOC_KEYS) or not all(is_int(a[k]) for k in ALLOC_KEYS):
            raise Malformed(f'{path}: allocation entry {a!r} has the wrong keys/types')
    _ascending([a['id'] for a in s['allocations']], 'allocations')
    for h in s['handles'] if isinstance(s['handles'], list) else [None]:
        if not isinstance(h, dict) or set(h) != {'handle', 'record', 'image', 'palettes'} or not is_int(h['handle']) \
                or not is_int(h['record']) or not isinstance(h['palettes'], list) or not h['palettes']:
            raise Malformed(f'{path}: handle entry {h!r} has the wrong keys/types')
        _pair(h['image'], 'handle image')
        for p in h['palettes']:
            _pair(p, 'handle palette')
    _ascending([h['handle'] for h in s['handles']], 'handles')
    for r in s['records'] if isinstance(s['records'], list) else [None]:
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
    for key in STATE_KEYS:
        if key == 'coverage':
            continue
        a, b = orig[key], nat[key]
        if canon(a) == canon(b):
            n = f' ({len(a)})' if isinstance(a, list) else ''
            print(f'  state.{key}: identical{n}', file=out)
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
        print(f'  state.{key}: DIFFERENT {detail}'[:600], file=out)
    return diffs

# ---- VRAM ------------------------------------------------------------------------------------------
def read_exact(path, n):
    try:
        data = Path(path).read_bytes()
    except OSError as exc:
        raise Malformed(str(exc))
    if len(data) != n:
        raise Malformed(f'{path}: {len(data)} bytes, expected {n}')
    return data

def compare_vram(orig_dir, nat_dir, out):
    mask = read_exact(Path(orig_dir) / 'vram-written-mask.bin', VRAM)
    values = read_exact(Path(orig_dir) / 'vram-written-values.bin', VRAM)
    inval = read_exact(Path(orig_dir) / 'vram-invalidated-mask.bin', VRAM)
    known = read_exact(Path(nat_dir) / 'vram.known', VRAM)
    data = read_exact(Path(nat_dir) / 'vram.bytes', VRAM)
    if any(b & ~3 for b in set(known)):
        raise Malformed('vram.known uses bits other than bit0/bit1')
    if any(m & ~3 for m in set(mask)) or any(m & ~3 for m in set(inval)):
        raise Malformed('original masks use bits other than bit0/bit1')
    counts = {'required_known': 0, 'required_unknown': 0, 'missing_knowledge': 0, 'value_mismatch': 0,
              'stale_known': 0, 'invented_knowledge': 0}
    first = {}
    for i in range(VRAM):
        m, t, k = mask[i], inval[i], known[i]
        if not (m | t | k):
            continue
        for bit, shift in ((1, 0), (2, 4)):
            if m & bit and t & bit:
                raise Malformed(f'original masks overlap at 0x{i:x}')
            if m & bit:
                counts['required_known'] += 1
                if not k & bit:
                    kind = 'missing_knowledge'
                elif ((values[i] >> shift) ^ (data[i] >> shift)) & 0xf:
                    kind = 'value_mismatch'
                else:
                    continue
            elif t & bit:
                counts['required_unknown'] += 1
                if not k & bit:
                    continue
                kind = 'stale_known'
            elif k & bit:
                kind = 'invented_knowledge'
            else:
                continue
            counts[kind] += 1
            first.setdefault(kind, f'0x{i:06x}.{"lo" if bit == 1 else "hi"}')
    bad = sum(counts[k] for k in ('missing_knowledge', 'value_mismatch', 'stale_known', 'invented_knowledge'))
    print('  vram: ' + ', '.join(f'{k}={v}' for k, v in counts.items())
          + (('; first ' + ', '.join(f'{k} at {v}' for k, v in first.items())) if first else ''), file=out)
    return int(bool(bad)), counts, first

# ---- pins ------------------------------------------------------------------------------------------
def verify_original(orig_dir, manifest_path):
    try:
        manifest = parse_json(Path(manifest_path).read_text(), str(manifest_path))
    except OSError as exc:
        raise Malformed(f'manifest: {exc}')
    outputs = manifest.get('outputs') if isinstance(manifest, dict) else None
    digest = manifest.get('result_digest') if isinstance(manifest, dict) else None
    if not isinstance(outputs, dict) or not isinstance(digest, dict):
        raise Malformed('original manifest lacks outputs/result_digest')
    for name in ORIGINAL_FILES:
        pin = outputs.get('analysis/' + name)
        if not isinstance(pin, dict):
            raise Malformed(f'manifest does not pin analysis/{name}')
        try:
            data = (Path(orig_dir) / name).read_bytes()
        except OSError as exc:
            raise Malformed(str(exc))
        if hashlib.sha256(data).hexdigest() != pin.get('sha256') or len(data) != pin.get('bytes'):
            raise Malformed(f'{name} does not match the pinned original capture in the manifest')
    return digest

def refuse_self_compare(orig_dir, nat_dir):
    o, n = Path(orig_dir).resolve(), Path(nat_dir).resolve()
    if o == n or o in n.parents or n in o.parents:
        raise Malformed('refusing to compare the original analysis with itself (same or nested directory)')
    for nn in ('timeline.jsonl', 'state.json', 'vram.bytes', 'vram.known'):
        p = Path(nat_dir) / nn
        for on in ORIGINAL_FILES:
            q = Path(orig_dir) / on
            if p.exists() and q.exists() and os.path.samefile(p, q):
                raise Malformed(f'refusing self-comparison: native {nn} is the original {on}')

def run(orig_dir, nat_dir, out=None, manifest=MANIFEST):
    out = out or sys.stdout
    refuse_self_compare(orig_dir, nat_dir)
    digest = verify_original(orig_dir, manifest)
    otl = load_timeline(Path(orig_dir) / 'events.jsonl')
    ntl = load_timeline(Path(nat_dir) / 'timeline.jsonl')
    ost = load_state(Path(orig_dir) / 'final-state.json')
    nst = load_state(Path(nat_dir) / 'state.json')
    exp = digest.get('resource_events')
    if exp != sum(1 for e in otl if e['kind'] != 'phase'):
        raise Malformed('manifest result digest does not match the original timeline length')
    print('Title logo/title construction, original vs native:', file=out)
    t, first = compare_timeline(otl, ntl, out)
    s = compare_state(ost, nst, out)
    v, vcounts, vfirst = compare_vram(orig_dir, nat_dir, out)
    if vcounts['required_known'] != digest.get('vram_known_nibbles') or vcounts['required_unknown'] != digest.get('vram_unknown_nibbles'):
        raise Malformed('original VRAM masks do not carry the manifest nibble counts')
    ignored = sorted(set(nst) & set(IGNORED_STATE_KEYS))
    print('  not compared: phase markers (informational)' + (f'; ignored native keys {ignored}' if ignored else '')
          + '; RNG (tools/rng_compare.py: no rand() call occurs after 0x100618 up to the stop in the original)', file=out)
    result = {'timeline_identical': t == 0, 'first_timeline_difference': first, 'state_differences': s,
              'vram': vcounts, 'vram_first': vfirst, 'identical': not (t or s or v)}
    print('RESULT: ' + ('IDENTICAL' if result['identical'] else 'DIFFERENT'), file=out)
    return (0 if result['identical'] else 1), result

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
