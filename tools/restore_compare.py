#!/usr/bin/env python3
"""Compare a native Session Restore-cancel report with the unchanged-retail Restore original reference.

usage: restore_compare.py ORIGINAL_ANALYSIS_DIR NATIVE_DIR [--manifest PATH] [--json OUT] [--strict]

ORIGINAL_ANALYSIS_DIR is the analysis/ directory written by tools/restore_original_extract.py. Every original
file used must equal its sha256/size pin in docs/restore-original-manifest.json (or --manifest), and the
manifest result digest must describe it, or the run is refused (exit 2).

NATIVE_DIR (the Retire layout, extended minimally; the Restore native report layout):
  route.json   {"route": "restore-cancel", "elf_sha256": "<pinned SCUS_971.29 sha256>",
                "checkpoints": {"retire-successor-entry": F, "restore-constructor-return": F, "first-layout": F,
                                "empty-ready": F, "cancel-death": F, "resource-retirement": F, "wrapper-free": F,
                                "second-title-return": F}}
               exactly these keys; F = native frame (integer >= 0), NON-DECREASING in that order (the constructor
               return and the first layout share the construction traversal). Frames are reported, never compared.
  manager-frames.jsonl
               one row per Restore traversal, t = 0 (construction traversal S) .. D (death):
               {"t","state_in","state_out","update_state_out","gate_open","pressed","draws"}
               state_in/state_out = *0x1f89a8 at entry/return of the input callback (0x11e480; null if it did not
               run), update_state_out = at the return of 0x11c2c8, gate_open = the fade guard let the input switch
               run, pressed = the 0x1c0160 word read by that call (null if not read), draws = Session rand draws in
               that traversal. Coverage is checked on BOTH sides independently: t must be contiguous from 0, row 0
               must start in state 0, exactly one Cancel (pressed 0x20) and exactly one death row (state_in < 0,
               state_out 0) which must be the last row. A missing interior/tail row fails even if both sides lack it.
  <checkpoint>/ for each of the eight checkpoints:
    lifecycle.jsonl  the ordered Session event stream FROM COLD BOOT up to the checkpoint (entry checkpoints precede
                     the call's events; return checkpoints follow them). Retire kinds/keys plus
                       {"kind":"upload-bytes","alloc":a,"bytes":n,"sha256":h}   bytes sent by each upload
                       {"kind":"mode","op":"destroy"|"resource-retire","mode":8|10,"selector":s}
                       {"kind":"mode","op":"wrapper-free","node":"resource"|"mode8"|"mode10"|"manager","mode":m,"selector":s}
                       {"kind":"ui","op":"manager-create","mode":10,"windows":n}  {"kind":"ui","op":"window-create","window":h}
                       {"kind":"ui","op":"state","state":s}  {"kind":"ui","op":"window-death","window":h}
                       {"kind":"ui","op":"manager-destroy","mode":10}  {"kind":"glyph","op":"cache-transfer"}
                     Each row may carry "asserted": true (PROVENANCE: a native owner assertion, not an observation of
                     an actual object). Asserted rows are compared like any row but reported separately and never
                     count as observation evidence: a comparison that is otherwise identical but uses any asserted row
                     is UNPROVEN.
    state.json       the Retire state schema with coverage {"lifecycle":"restore","checkpoint":cp,"memory":...}
    effects.json     {"effect106": ...} as Retire
    rng.jsonl        {"i","phase","state","value"} (+ optional "asserted": true); phases noise|effect-grid|
                     effect-template|session|restore-ui-glyph|restore-ui-icon|restore-ui-select
    vram.bytes, vram.known, optional vram.invalidated   as Retire

Result (exit status): 0 IDENTICAL; 1 DIFFERENT (any lifecycle/state/effect/VRAM difference, stream inconsistency or
incomplete coverage, or per-traversal/RNG differences with equal timing anchors); 2 MALFORMED/unpinned/identity
mismatch/self-comparison; 3 TIMING-DIFFERENT (every checkpoint's lifecycle/state/effect/VRAM is identical, but the
Restore timing anchors (empty-ready, Cancel or death traversal, number of traversals, or the card-readiness
traversals card_info_t/card_ready_t, which are IO/observer dependent) differ, AND the timing diagnostic holds: the
RNG through first-layout identical, no draw after the window dies, manager-frame draw counts accounting for every
post-stop draw, and the same set of displayed statuses with ONE identical call-site-label pass per status. Per-traversal and RNG parity are then UNRESOLVED: the label is inconclusive about other
behaviour and never a fixed-frame equality. If the diagnostic fails the result is DIFFERENT);
4 UNPROVEN (identical only through asserted rows). Research tool; never product input.
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import retire_title_compare as base  # noqa: E402

MANIFEST = Path(__file__).resolve().parents[1] / 'docs/restore-original-manifest.json'
VRAM = 1 << 22
ROUTE = 'restore-cancel'
CHECKPOINTS = ('retire-successor-entry', 'restore-constructor-return', 'first-layout', 'empty-ready', 'cancel-death',
               'resource-retirement', 'wrapper-free', 'second-title-return')
MEMORY = base.MEMORY
RNG_PHASES = base.RNG_PHASES + ('restore-ui-glyph', 'restore-ui-icon', 'restore-ui-select')
EVENT_KEYS = dict(base.EVENT_KEYS)
EVENT_KEYS['upload-bytes'] = {'alloc': int, 'bytes': int, 'sha256': str}
OP_KEYS = {
    'effect': base.OP_KEYS['effect'],
    'mode': {
        'request': {'mode': int, 'selector': int, 'arg': int},
        'resource-retire': {'mode': int, 'selector': int},
        'destroy': {'mode': int, 'selector': int},
        'wrapper-free': {'node': ('resource', 'mode8', 'mode10', 'manager'), 'mode': int, 'selector': int},
    },
    'ui': {
        'manager-create': {'mode': int, 'windows': int},
        'window-create': {'window': int},
        'state': {'state': int},
        'window-death': {'window': int},
        'manager-destroy': {'mode': int},
    },
    'glyph': {'cache-transfer': {}},
}
STATE_KEYS = base.STATE_KEYS
IGNORED_STATE_KEYS = base.IGNORED_STATE_KEYS
ALLOC_KEYS = base.ALLOC_KEYS
FRAME_KEYS = ('t', 'state_in', 'state_out', 'update_state_out', 'gate_open', 'pressed', 'draws')
ORIGINAL_FILES = ('events.jsonl', 'state.json', 'effects.json', 'rng.jsonl', 'vram-written-mask.bin',
                  'vram-written-values.bin', 'vram-invalidated-mask.bin')
NATIVE_FILES = ('lifecycle.jsonl', 'state.json', 'effects.json', 'rng.jsonl', 'vram.bytes', 'vram.known', 'vram.invalidated')

Malformed = base.Malformed
parse_json = base.parse_json
read_text = base.read_text
is_int = base.is_int
typed = base.typed
canon = base.canon
jsonl_rows = base.jsonl_rows

def _asserted(e, where):
    if 'asserted' in e:
        if e['asserted'] is not True:
            raise Malformed(f'{where}: "asserted" must be true when present')
        e = dict(e)
        del e['asserted']
        return e, True
    return e, False

# ---- lifecycle -------------------------------------------------------------------------------------
def load_lifecycle(path, native=False):
    rows, asserted = [], []
    for n, e in jsonl_rows(path):
        if not isinstance(e, dict):
            raise Malformed(f'{path}:{n}: not an object')
        a = False
        if native:
            e, a = _asserted(e, f'{path}:{n}')
        elif 'asserted' in e:
            raise Malformed(f'{path}:{n}: original rows are observations and never carry "asserted"')
        if e.get('kind') not in set(EVENT_KEYS) | set(OP_KEYS):
            raise Malformed(f'{path}:{n}: unknown event kind {e.get("kind")!r}')
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
        if e['kind'] == 'upload-bytes' and (len(e['sha256']) != 64 or any(c not in '0123456789abcdef' for c in e['sha256'])):
            raise Malformed(f'{path}:{n}: upload-bytes sha256 is not 64 lower-case hex digits')
        rows.append(e)
        asserted.append(a)
    if not any(r['kind'] != 'phase' for r in rows):
        raise Malformed(f'{path}: no lifecycle events')
    return rows, asserted

def kind_key(e):
    return e['kind'] + (':' + e['op'] if 'op' in e else '')

def compare_events(orig, nat, nat_asserted, out):
    a = [e for e in orig if e['kind'] != 'phase']
    pairs = [(e, s) for e, s in zip(nat, nat_asserted) if e['kind'] != 'phase']
    b = [e for e, _ in pairs]
    kinds = sorted(set(kind_key(e) for e in a + b))
    ca = {k: sum(1 for e in a if kind_key(e) == k) for k in kinds}
    cb = {k: sum(1 for e in b if kind_key(e) == k) for k in kinds}
    print('    event counts original: ' + ', '.join(f'{k}={ca[k]}' for k in kinds), file=out)
    if ca != cb:
        print('    event counts native:   ' + ', '.join(f'{k}={cb[k]}' for k in kinds), file=out)
    asserted = sum(1 for _, s in pairs if s)
    first = None
    for i in range(max(len(a), len(b))):
        if i >= len(a) or i >= len(b) or canon(a[i]) != canon(b[i]):
            first = i
            break
    if first is None:
        observed = len(b) - asserted
        print(f'    events: identical ({len(a)} in order; {observed} observed, {asserted} owner-asserted)', file=out)
        return 0, None, asserted
    print(f'    events: DIFFERENT at event {first} (of {len(a)} original / {len(b)} native)', file=out)
    for i in range(max(0, first - 2), min(max(len(a), len(b)), first + 3)):
        x = canon(a[i]) if i < len(a) else '<absent>'
        y = canon(b[i]) if i < len(b) else '<absent>'
        tag = ' [asserted]' if i < len(pairs) and pairs[i][1] else ''
        print(f'      [{i}] {"==" if x == y else "!="} original {x} | native {y}{tag}', file=out)
    return 1, first, asserted

def load_state(path, cp):
    s = parse_json(read_text(path), str(path))
    if not isinstance(s, dict):
        raise Malformed(f'{path}: not an object')
    cov = {'lifecycle': 'restore', 'checkpoint': cp, 'memory': MEMORY}
    if s.get('coverage') != cov:
        raise Malformed(f'{path}: coverage {s.get("coverage")!r} != {cov!r}')
    tmp = dict(s)
    tmp['coverage'] = {'lifecycle': 'retire-title', 'checkpoint': cp, 'memory': MEMORY}
    saved = base.read_text
    base.read_text = lambda _p: json.dumps(tmp)
    try:
        base.load_state(path, cp)
    finally:
        base.read_text = saved
    return s

def load_rng(path, native=False):
    rows, asserted = [], []
    for n, d in jsonl_rows(path):
        if not isinstance(d, dict):
            raise Malformed(f'{path}:{n}: rng row')
        a = False
        if native:
            d, a = _asserted(d, f'{path}:{n}')
        if set(d) != {'i', 'phase', 'state', 'value'}:
            raise Malformed(f'{path}:{n}: rng row keys')
        if not (is_int(d['i']) and is_int(d['state']) and is_int(d['value'])) or d['phase'] not in RNG_PHASES \
                or not 0 <= d['state'] <= 0xffffffff or not 0 <= d['value'] <= 0xffffffff or d['i'] != len(rows):
            raise Malformed(f'{path}:{n}: rng row {d!r}')
        rows.append(d)
        asserted.append(a)
    return rows, asserted

def compare_rng(orig, nat, nat_asserted, out):
    for i in range(max(len(orig), len(nat))):
        if i >= len(orig) or i >= len(nat) or orig[i] != nat[i]:
            print(f'    rng: DIFFERENT at draw {i} (of {len(orig)} original / {len(nat)} native): original '
                  f'{orig[i] if i < len(orig) else "<absent>"} | native {nat[i] if i < len(nat) else "<absent>"}', file=out)
            return 1, i, sum(nat_asserted)
    n = sum(nat_asserted)
    print(f'    rng: identical ({len(orig)} draws; {n} owner-asserted)', file=out)
    return 0, None, n

def compare_vram(orig_dir, nat_dir, out):
    saved = base.VRAM
    base.VRAM = VRAM
    try:
        return base.compare_vram(orig_dir, nat_dir, out)
    finally:
        base.VRAM = saved

def load_frames(path, native):
    rows, asserted = [], []
    for n, r in jsonl_rows(path):
        if not isinstance(r, dict):
            raise Malformed(f'{path}:{n}: not an object')
        a = False
        if native:
            r, a = _asserted(r, f'{path}:{n}')
            if set(r) != set(FRAME_KEYS):
                raise Malformed(f'{path}:{n}: manager-frames keys {sorted(r)} != {sorted(FRAME_KEYS)}')
        else:
            missing = set(FRAME_KEYS) - set(r)
            if missing:
                raise Malformed(f'{path}:{n}: original manager frame lacks {sorted(missing)}')
            r = {k: r[k] for k in FRAME_KEYS}
        for k in ('t', 'draws'):
            if not is_int(r[k]) or r[k] < 0:
                raise Malformed(f'{path}:{n}: {k} must be a non-negative integer')
        for k in ('state_in', 'state_out', 'update_state_out', 'pressed'):
            if r[k] is not None and not is_int(r[k]):
                raise Malformed(f'{path}:{n}: {k} must be an integer or null')
        if r['gate_open'] is not None and type(r['gate_open']) is not bool:
            raise Malformed(f'{path}:{n}: gate_open must be a boolean or null')
        rows.append(r)
        asserted.append(a)
    if not rows:
        raise Malformed(f'{path}: no manager frames')
    return rows, asserted

def frame_coverage(rows):
    """Expected intervals recovered from the stream's own route events (construction, Cancel, death), never from
    the other side: returns a list of coverage problems."""
    bad = []
    ts = [r['t'] for r in rows]
    if ts[0] != 0:
        bad.append(f'first row is t={ts[0]}, not the construction traversal 0')
    holes = [t for t in range(ts[0], ts[-1] + 1) if t not in set(ts)]
    if holes:
        bad.append(f'missing traversals {holes[:8]}{"..." if len(holes) > 8 else ""}')
    if len(set(ts)) != len(ts) or ts != sorted(ts):
        bad.append('traversals duplicated or out of order')
    if rows[0]['state_in'] != 0:
        bad.append(f'row 0 does not start in state 0 (state_in {rows[0]["state_in"]})')
    cancel = [r['t'] for r in rows if r['pressed'] == 0x20]
    if len(cancel) != 1:
        bad.append(f'expected exactly one Cancel (pressed 0x20), found {len(cancel)}')
    death = [r['t'] for r in rows if r['state_in'] is not None and r['state_in'] < 0 and r['state_out'] == 0]
    if len(death) != 1:
        bad.append(f'expected exactly one death row, found {len(death)}')
    elif death[0] != ts[-1]:
        bad.append(f'rows continue after the death traversal {death[0]} (last {ts[-1]}) or end before it')
    missing_input = [r['t'] for r in rows if r['state_in'] is None]
    if missing_input:
        bad.append(f'input callback missing in traversals {missing_input[:8]}')
    return bad

def anchors(rows):
    ready = next((r['t'] for r in rows if r['state_in'] == 0x21c and r['gate_open']), None)
    cancel = next((r['t'] for r in rows if r['pressed'] == 0x20), None)
    death = next((r['t'] for r in rows if r['state_in'] is not None and r['state_in'] < 0 and r['state_out'] == 0), None)
    return {'empty_ready_t': ready, 'cancel_t': cancel, 'death_t': death, 'traversals': len(rows)}

def readiness(rows):
    """Card readiness traversals (Restore lead): first 11c2c8 return in 0x212 (card info done) and in 0x21c
    (slot scan done). Observer/IO dependent in the original as well (report section 2.1)."""
    info = next((r['t'] for r in rows if r['update_state_out'] == 0x212), None)
    ready = next((r['t'] for r in rows if r['update_state_out'] == 0x21c), None)
    return {'card_info_t': info, 'card_ready_t': ready}

def pass_model(rows, rng_rows):
    """Map each traversal's RNG pass (call-site label sequence) to the status the layout shows: the
    constructor pass at t=0, else the 11c2c8 state two traversals earlier (text published by 11c2c8 is
    laid out two traversals later). Returns {status: labels} or None if one status has two patterns."""
    model, i = {}, 0
    for r in rows:
        t = r['t']
        key = 'constructor' if t == 0 else rows[max(t - 2, 0)]['update_state_out']
        labels = tuple(x['phase'] for x in rng_rows[i:i + r['draws']])
        i += r['draws']
        if model.setdefault(key, labels) != labels:
            return None
    return model

def timing_diagnostic(rng_at, o_fr, n_fr):
    """rng_at: {checkpoint: (original rng rows, native rng rows)}. Returns (checks, original pass model)."""
    fixed_cps = CHECKPOINTS[:CHECKPOINTS.index('first-layout') + 1]
    fixed_same = all(o == n for o, n in (rng_at[cp] for cp in fixed_cps))
    death = CHECKPOINTS.index('cancel-death')
    no_late = all(len(rng_at[cp][s]) == len(rng_at['cancel-death'][s]) for cp in CHECKPOINTS[death:] for s in (0, 1))
    o_all, n_all = rng_at[CHECKPOINTS[-1]]
    prefix = len(rng_at[CHECKPOINTS[0]][0])
    accounted = (sum(r['draws'] for r in o_fr) == len(o_all) - prefix and
                 sum(r['draws'] for r in n_fr) == len(n_all) - len(rng_at[CHECKPOINTS[0]][1]))
    mo, mn = pass_model(o_fr, o_all[prefix:]), pass_model(n_fr, n_all[prefix:])
    model_same = mo is not None and mn is not None and mo.keys() == mn.keys() and all(mo[k] == mn[k] for k in mo)
    diagnostic = {'fixed_checkpoints_rng_identical': fixed_same, 'no_draws_after_death': no_late,
                  'frame_draws_account_for_all': accounted, 'pass_model_identical': model_same}
    return diagnostic, mo

def compare_frames(orig, nat, out):
    first = next((i for i in range(max(len(orig), len(nat)))
                  if i >= len(orig) or i >= len(nat) or orig[i] != nat[i]), None)
    if first is None:
        print(f'  manager frames: identical ({len(orig)} traversals)', file=out)
    else:
        print(f'  manager frames: DIFFERENT at t={first} ({len(orig)} original / {len(nat)} native): original '
              f'{orig[first] if first < len(orig) else "<absent>"} | native {nat[first] if first < len(nat) else "<absent>"}',
              file=out)
    return first

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

def verify_pin(orig_dir, rel, manifest):
    pin = manifest['outputs'].get('analysis/' + rel)
    if not isinstance(pin, dict):
        raise Malformed(f'manifest does not pin analysis/{rel}')
    try:
        data = (Path(orig_dir) / rel).read_bytes()
    except OSError as exc:
        raise Malformed(str(exc))
    if hashlib.sha256(data).hexdigest() != pin.get('sha256') or len(data) != pin.get('bytes'):
        raise Malformed(f'analysis/{rel} does not match the pinned original capture in the manifest')

def verify_original(orig_dir, manifest):
    for cp in CHECKPOINTS:
        for name in ORIGINAL_FILES:
            verify_pin(orig_dir, f'{cp}/{name}', manifest)
    verify_pin(orig_dir, 'manager-frames.jsonl', manifest)

def load_route(nat_dir, elf):
    r = parse_json(read_text(Path(nat_dir) / 'route.json'), 'route.json')
    if not isinstance(r, dict) or set(r) != {'route', 'elf_sha256', 'checkpoints'}:
        raise Malformed('route.json must hold exactly route, elf_sha256, checkpoints')
    if r['route'] != ROUTE:
        raise Malformed(f'route.json: route {r["route"]!r} is not {ROUTE!r}')
    if r['elf_sha256'] != elf:
        raise Malformed('route.json: ELF identity differs from the pinned SCUS_971.29 (addresses would not apply)')
    c = r['checkpoints']
    if not isinstance(c, dict) or set(c) != set(CHECKPOINTS) or any(not is_int(v) or v < 0 for v in c.values()):
        raise Malformed(f'route.json: checkpoints must map exactly {CHECKPOINTS} to non-negative integer frames')
    frames = [c[k] for k in CHECKPOINTS]
    if any(b < a for a, b in zip(frames, frames[1:])):
        raise Malformed(f'route.json: checkpoint frames {frames} decrease (reordered checkpoint?)')
    return r

def refuse_self_compare(orig_dir, nat_dir):
    o, n = Path(orig_dir).resolve(), Path(nat_dir).resolve()
    if o == n or o in n.parents or n in o.parents:
        raise Malformed('refusing to compare the original analysis with itself (same or nested directory)')
    originals = [p for p in o.rglob('*') if p.is_file()]
    natives = [n / cp / nn for cp in CHECKPOINTS for nn in NATIVE_FILES] + [n / 'manager-frames.jsonl']
    for p in natives:
        if not p.exists():
            continue
        for q in originals:
            if os.path.samefile(p, q):
                raise Malformed(f'refusing self-comparison: native {p.relative_to(n)} is the original {q.relative_to(o)}')

def run(orig_dir, nat_dir, out=None, manifest=MANIFEST, strict=False):
    out = out or sys.stdout
    refuse_self_compare(orig_dir, nat_dir)
    m, elf = load_manifest(manifest)
    route = load_route(nat_dir, elf)
    verify_original(orig_dir, m)
    digest = m['result_digest']
    if not isinstance(digest.get('checkpoints'), dict) or not isinstance(digest.get('manager_frames'), dict):
        raise Malformed('manifest result digest lacks checkpoints/manager_frames')
    loaded = {}
    for cp in CHECKPOINTS:
        od, nd = Path(orig_dir) / cp, Path(nat_dir) / cp
        if not nd.is_dir():
            raise Malformed(f'native checkpoint directory {cp} missing')
        o_ev, _ = load_lifecycle(od / 'events.jsonl')
        n_ev, n_as = load_lifecycle(nd / 'lifecycle.jsonl', native=True)
        o_rng, _ = load_rng(od / 'rng.jsonl')
        n_rng, n_ras = load_rng(nd / 'rng.jsonl', native=True)
        loaded[cp] = {'o_ev': o_ev, 'n_ev': n_ev, 'n_as': n_as,
                      'o_st': load_state(od / 'state.json', cp), 'n_st': load_state(nd / 'state.json', cp),
                      'o_fx': base.load_effects(od / 'effects.json'), 'n_fx': base.load_effects(nd / 'effects.json'),
                      'o_rng': o_rng, 'n_rng': n_rng, 'n_ras': n_ras}
        d = digest['checkpoints'].get(cp)
        if not isinstance(d, dict) or d.get('resource_events') != sum(1 for e in o_ev if e['kind'] != 'phase') \
                or d.get('rng_draws') != len(o_rng):
            raise Malformed(f'manifest result digest does not match the original {cp}')
    o_fr, _ = load_frames(Path(orig_dir) / 'manager-frames.jsonl', native=False)
    if frame_coverage(o_fr):
        raise Malformed(f'original manager frames are incomplete: {frame_coverage(o_fr)}')
    if digest['manager_frames'] != anchors(o_fr):
        raise Malformed('manifest result digest does not match the original manager frames')
    fpath = Path(nat_dir) / 'manager-frames.jsonl'
    if not fpath.exists():
        raise Malformed('native manager-frames.jsonl missing')
    n_fr, n_fas = load_frames(fpath, native=True)

    print('Restore cancel, original vs native:', file=out)
    result = {'route': ROUTE, 'native_frames': route['checkpoints'], 'checkpoints': {}}
    payload_bad = False
    rng_bad = False
    asserted_total = {'lifecycle': 0, 'rng': 0, 'manager_frames': sum(n_fas)}
    prev = None
    for cp in CHECKPOINTS:
        L = loaded[cp]
        print(f'  [{cp}] native frame {route["checkpoints"][cp]}', file=out)
        e, first, ea = compare_events(L['o_ev'], L['n_ev'], L['n_as'], out)
        s = base.compare_state(L['o_st'], L['n_st'], out)
        fx = int(canon(L['o_fx']) != canon(L['n_fx']))
        print(f'    effect106: DIFFERENT original {L["o_fx"]} | native {L["n_fx"]}' if fx
              else f'    effect106: identical {L["o_fx"]}', file=out)
        r, rfirst, ra = compare_rng(L['o_rng'], L['n_rng'], L['n_ras'], out)
        v, vcounts, vfirst = compare_vram(Path(orig_dir) / cp, Path(nat_dir) / cp, out)
        d = digest['checkpoints'][cp]
        if vcounts['required_known'] != d.get('vram_known_nibbles') or vcounts['required_unknown'] != d.get('vram_unknown_nibbles'):
            raise Malformed(f'original VRAM masks of {cp} do not carry the manifest nibble counts')
        inconsistent = []
        if prev is not None:
            pe = [x for x in loaded[prev]['n_ev'] if x['kind'] != 'phase']
            ne = [x for x in L['n_ev'] if x['kind'] != 'phase']
            if [canon(x) for x in ne[:len(pe)]] != [canon(x) for x in pe] or len(ne) <= len(pe):
                inconsistent.append('lifecycle')
            if L['n_rng'][:len(loaded[prev]['n_rng'])] != loaded[prev]['n_rng']:
                inconsistent.append('rng')
        if inconsistent:
            print(f'    stream_inconsistent: native {inconsistent} do not strictly extend checkpoint {prev}', file=out)
        asserted_total['lifecycle'] += ea
        asserted_total['rng'] += ra
        payload_bad |= bool(e or s or fx or v or inconsistent)
        rng_bad |= bool(r)
        result['checkpoints'][cp] = {'events_identical': not e, 'first_event_difference': first, 'state_differences': s,
                                     'effect106_identical': not fx, 'rng_identical': not r, 'first_rng_difference': rfirst,
                                     'vram': vcounts, 'vram_first': vfirst, 'stream_inconsistent': inconsistent,
                                     'asserted_events': ea, 'asserted_draws': ra,
                                     'identical': not (e or s or fx or r or v or inconsistent)}
        prev = cp
    cov = frame_coverage(n_fr)
    if cov:
        print(f'  manager frames: INCOMPLETE native coverage: {cov}', file=out)
        payload_bad = True
    ffirst = compare_frames(o_fr, n_fr, out)
    ao, an = anchors(o_fr), anchors(n_fr)
    ro, rn = readiness(o_fr), readiness(n_fr)
    timing_differs = ao != an or ro != rn
    print(f'  timing anchors: original {ao} {ro} | native {an} {rn}' + (' (DIFFERENT)' if timing_differs else ''), file=out)
    rng_at = {cp: (loaded[cp]['o_rng'], loaded[cp]['n_rng']) for cp in CHECKPOINTS}
    diagnostic, mo = timing_diagnostic(rng_at, o_fr, n_fr)
    diagnostic_ok = all(diagnostic.values())
    print(f'  timing diagnostic: {diagnostic}'
          + (f' (draws per pass: {dict((str(k), len(v)) for k, v in mo.items())})' if mo else ''), file=out)
    gaps = sum(c['vram']['coverage_gap_invalidated'] for c in result['checkpoints'].values())
    if strict and gaps:
        print(f'  strict: {gaps} coverage gaps are not accepted', file=out)
        payload_bad = True
    n_assert = sum(asserted_total.values())
    print(f'  provenance: owner-asserted rows lifecycle={asserted_total["lifecycle"]} rng={asserted_total["rng"]} '
          f'manager_frames={asserted_total["manager_frames"]} (compared, reported separately, never observation evidence)',
          file=out)
    print(f'  not compared: phase rows, native frame numbers vs original vblanks; coverage gaps: {gaps}', file=out)
    per_frame_bad = rng_bad or ffirst is not None
    if payload_bad:
        label, code = 'DIFFERENT', 1
    elif per_frame_bad and timing_differs and diagnostic_ok:
        label, code = 'TIMING-DIFFERENT', 3
        print('  lifecycle/state/effect/VRAM are identical at every checkpoint; the Restore timing anchors differ, so '
              'per-traversal and RNG parity are UNRESOLVED (inconclusive about any other behaviour; not an equality)',
              file=out)
    elif per_frame_bad:
        label, code = 'DIFFERENT', 1
    elif n_assert:
        label, code = 'UNPROVEN', 4
        print('  every compared row matches, but owner-asserted rows were needed: not observation evidence', file=out)
    else:
        label, code = 'IDENTICAL', 0
    result.update({'manager_frames_first_difference': ffirst, 'manager_frames_coverage': cov, 'anchors_original': ao,
                   'anchors_native': an, 'readiness_original': ro, 'readiness_native': rn,
                   'timing_diagnostic': diagnostic, 'timing_differs': timing_differs, 'asserted': asserted_total,
                   'coverage_gaps': gaps, 'result': label})
    print('RESULT: ' + label, file=out)
    return code, result

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('original')
    p.add_argument('native')
    p.add_argument('--manifest', default=str(MANIFEST))
    p.add_argument('--json')
    p.add_argument('--strict', action='store_true', help='zero VRAM coverage gaps required')
    a = p.parse_args(argv)
    try:
        code, result = run(a.original, a.native, manifest=a.manifest, strict=a.strict)
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
