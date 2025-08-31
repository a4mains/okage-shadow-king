#!/usr/bin/env python3
"""Compare a native Session New Game report with the unchanged-retail Newgame original reference.

usage: newgame_compare.py ORIGINAL_ANALYSIS_DIR NATIVE_DIR [--manifest PATH] [--json OUT] [--strict]

ORIGINAL_ANALYSIS_DIR is the analysis/ directory written by tools/newgame_original_extract.py. Every original file
used must equal its sha256/size pin in docs/newgame-original-manifest.json (or --manifest), and the manifest result
digest must describe it, or the run is refused (exit 2).

NATIVE_DIR (`okage-native --newgame-report DIR`):
  route.json   {"route": "newgame-chapter", "elf_sha256": "<pinned SCUS_971.29 sha256>", "construction_frame": S,
                "checkpoints": {<the 16 CHECKPOINTS>: F}, "tails": {"theme_end_t": T|null}}
               exactly these keys; F = native frame, non-decreasing in CHECKPOINTS order; t = F - S is a timing ANCHOR.
  name-frames.jsonl     one row per name-entry wrapper traversal, t = 0 (construction traversal S) onward:
               {"t","manager_ran","phase_in","phase_out","length_out","selection_out","yes_no_out","pressed","repeat",
                "draws","name_out"} (manager fields null when the type-9 manager did not run in that traversal)
  chapter-frames.jsonl  one row per 0x133a98 call: {"t","counter_out","phase_out","fade_current_in","fade_target_in",
                "fade_target_out","command_out","image_counter_in","draws"}
  <checkpoint>/ for each of the 16 checkpoints:
    lifecycle.jsonl  the ordered Session event stream FROM COLD BOOT up to the checkpoint (Restore vocabulary plus
                     {"kind":"mode","op":"command","command":[a,b,c]}; owner modes 8/9/2; wrapper-free nodes
                     resource|mode8|mode9|mode2|manager). Rows may carry "asserted": true (owner assertion, never
                     observation evidence: a comparison identical only through asserted rows is UNPROVEN).
    state.json       the Retire state schema with coverage {"lifecycle":"newgame","checkpoint":cp,"memory":...}
    effects.json     {"effect106": ...}
    rng.jsonl        {"i","phase","state","value"}; phases noise|effect-grid|effect-template|session|name-ui-glyph|
                     name-ui-icon|name-ui-select
    session.json     {"image_counter","command","name_844","temp_10_sha256","fade":{current,target,rate,color},
                      "mode_parent":{"phase","queue","current"}}
    vram.bytes, vram.known, vram.invalidated   as Retire

Coverage is derived from EACH stream's own route events (never from the other side): name frames contiguous from
t = 0 (manager, phase 0) through exactly one death row (the type-9 manager's last call) followed by exactly two
manager-less rows (unlink, publication); chapter frames contiguous with counters 1..N, exactly one 240, the command
cleared only in the last row. A missing row fails even if both sides lack it.

Result (exit status): 0 IDENTICAL; 1 DIFFERENT (anchors equal and any lifecycle/state/effect/RNG/VRAM/session or
per-traversal difference, or a stream inconsistency); 2 MALFORMED/unpinned/identity mismatch/self-comparison;
3 ANCHORS-DIFFER (a timing anchor - checkpoint t, press/death traversal, chapter 240/clear traversal, theme end -
differs: the comparison is INCONCLUSIVE; payload differences are listed but never attributed to timing);
4 UNPROVEN (identical only through asserted rows); 5 INCOMPLETE (native coverage missing; always a failure).
Research tool; never product input.
"""
import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import retire_title_compare as base  # noqa: E402
import restore_compare as g  # noqa: E402

MANIFEST = Path(__file__).resolve().parents[1] / 'docs/newgame-original-manifest.json'
ROUTE = 'newgame-chapter'
CHECKPOINTS = ('retire-successor-entry', 'name-constructor-return', 'first-layout', 'name-edited', 'name-confirmed',
               'name-death', 'manager-free', 'command-published', 'resource-retirement', 'wrapper-free',
               'chapter-constructor-return', 'chapter-fade-request', 'chapter-mode-request',
               'chapter-resource-retirement', 'chapter-wrapper-free', 'field-constructor-entry')
MEMORY = base.MEMORY
RNG_PHASES = base.RNG_PHASES + ('name-ui-glyph', 'name-ui-icon', 'name-ui-select')
EVENT_KEYS = dict(g.EVENT_KEYS)
OP_KEYS = {
    'effect': base.OP_KEYS['effect'],
    'mode': {
        'request': {'mode': int, 'selector': int, 'arg': int},
        'resource-retire': {'mode': int, 'selector': int},
        'destroy': {'mode': int, 'selector': int},
        'wrapper-free': {'node': ('resource', 'mode8', 'mode9', 'mode2', 'manager'), 'mode': int, 'selector': int},
        'command': {'command': 'int3'},
    },
    'ui': {
        'manager-create': {'mode': int, 'windows': int},
        'window-create': {'window': int},
        'window-death': {'window': int},
        'manager-destroy': {'mode': int},
    },
    'glyph': {'cache-transfer': {}},
}
NAME_KEYS = ('t', 'manager_ran', 'phase_in', 'phase_out', 'length_out', 'selection_out', 'yes_no_out', 'pressed',
             'repeat', 'draws', 'name_out')
CHAPTER_KEYS = ('t', 'counter_out', 'phase_out', 'fade_current_in', 'fade_target_in', 'fade_target_out', 'command_out',
                'image_counter_in', 'draws')
SESSION_KEYS = ('image_counter', 'command', 'name_844', 'temp_10_sha256', 'fade', 'mode_parent')
ORIGINAL_FILES = ('events.jsonl', 'state.json', 'effects.json', 'rng.jsonl', 'session.json', 'vram-written-mask.bin',
                  'vram-written-values.bin', 'vram-invalidated-mask.bin')
NATIVE_FILES = ('lifecycle.jsonl', 'state.json', 'effects.json', 'rng.jsonl', 'session.json', 'vram.bytes', 'vram.known',
                'vram.invalidated')

Malformed = base.Malformed
parse_json = base.parse_json
read_text = base.read_text
is_int = base.is_int
canon = base.canon
jsonl_rows = base.jsonl_rows

def _typed(v, t):
    if t == 'int3':
        return isinstance(v, list) and len(v) == 3 and all(is_int(x) and 0 <= x <= 0xffffffff for x in v)
    return base.typed(v, t)

# ---- lifecycle -------------------------------------------------------------------------------------
def load_lifecycle(path, native=False):
    rows, asserted = [], []
    for n, e in jsonl_rows(path):
        if not isinstance(e, dict):
            raise Malformed(f'{path}:{n}: not an object')
        a = False
        if native:
            e, a = g._asserted(e, f'{path}:{n}')
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
            if not _typed(e[k], t):
                raise Malformed(f'{path}:{n}: {e["kind"]}.{k} = {e[k]!r} has the wrong type/value')
        if e['kind'] == 'upload-bytes' and (len(e['sha256']) != 64 or any(c not in '0123456789abcdef' for c in e['sha256'])):
            raise Malformed(f'{path}:{n}: upload-bytes sha256 is not 64 lower-case hex digits')
        rows.append(e)
        asserted.append(a)
    if not any(r['kind'] != 'phase' for r in rows):
        raise Malformed(f'{path}: no lifecycle events')
    return rows, asserted

def load_state(path, cp):
    s = parse_json(read_text(path), str(path))
    if not isinstance(s, dict):
        raise Malformed(f'{path}: not an object')
    cov = {'lifecycle': 'newgame', 'checkpoint': cp, 'memory': MEMORY}
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
            d, a = g._asserted(d, f'{path}:{n}')
        if set(d) != {'i', 'phase', 'state', 'value'}:
            raise Malformed(f'{path}:{n}: rng row keys')
        if not (is_int(d['i']) and is_int(d['state']) and is_int(d['value'])) or d['phase'] not in RNG_PHASES \
                or not 0 <= d['state'] <= 0xffffffff or not 0 <= d['value'] <= 0xffffffff or d['i'] != len(rows):
            raise Malformed(f'{path}:{n}: rng row {d!r}')
        rows.append(d)
        asserted.append(a)
    return rows, asserted

def _hexword(v):
    return isinstance(v, str) and v.startswith('0x') and len(v) <= 10 and all(c in '0123456789abcdef' for c in v[2:]) and len(v) > 2

def load_session(path):
    s = parse_json(read_text(path), str(path))
    if not isinstance(s, dict) or set(s) != set(SESSION_KEYS):
        raise Malformed(f'{path}: session keys {sorted(s) if isinstance(s, dict) else s!r} != {sorted(SESSION_KEYS)}')
    if not is_int(s['image_counter']) or not _typed(s['command'], 'int3'):
        raise Malformed(f'{path}: image_counter/command')
    if not isinstance(s['name_844'], str) or len(s['name_844']) != 32 or not isinstance(s['temp_10_sha256'], str) \
            or len(s['temp_10_sha256']) != 64:
        raise Malformed(f'{path}: name_844/temp_10_sha256')
    f = s['fade']
    if not isinstance(f, dict) or set(f) != {'current', 'target', 'rate', 'color'} or not all(_hexword(f[k]) for k in f):
        raise Malformed(f'{path}: fade must hold current/target/rate/color as 0x words')
    m = s['mode_parent']
    if not isinstance(m, dict) or set(m) != {'phase', 'queue', 'current'} or not is_int(m['phase']) \
            or not _typed(m['queue'], 'int3') or not _typed(m['current'], 'int3'):
        raise Malformed(f'{path}: mode_parent')
    return s

def compare_session(orig, nat, out):
    diffs = [k for k in SESSION_KEYS if canon(orig[k]) != canon(nat[k])]
    for k in diffs:
        print(f'    session.{k}: DIFFERENT original {orig[k]!r} | native {nat[k]!r}', file=out)
    if not diffs:
        print(f'    session: identical (image counter {orig["image_counter"]}, command {orig["command"]}, '
              f'fade {orig["fade"]["current"]}->{orig["fade"]["target"]})', file=out)
    return diffs

def load_frames(path, keys, native, what):
    rows, asserted = [], []
    for n, r in jsonl_rows(path):
        if not isinstance(r, dict):
            raise Malformed(f'{path}:{n}: not an object')
        a = False
        if native:
            r, a = g._asserted(r, f'{path}:{n}')
        if set(r) != set(keys):
            raise Malformed(f'{path}:{n}: {what} keys {sorted(r)} != {sorted(keys)}')
        if not is_int(r['t']) or r['t'] < 0 or not is_int(r['draws']) or r['draws'] < 0:
            raise Malformed(f'{path}:{n}: t/draws must be non-negative integers')
        rows.append(r)
        asserted.append(a)
    if not rows:
        raise Malformed(f'{path}: no {what}')
    return rows, asserted

def _contiguous(ts):
    bad = []
    holes = [t for t in range(ts[0], ts[-1] + 1) if t not in set(ts)]
    if holes:
        bad.append(f'missing traversals {holes[:8]}{"..." if len(holes) > 8 else ""}')
    if len(set(ts)) != len(ts) or ts != sorted(ts):
        bad.append('traversals duplicated or out of order')
    return bad

def name_coverage(rows):
    """Expected rows from the stream's own route events: construction (t = 0, manager in phase 0), the type-9 manager's
    calls up to its death call (phase -3, the last manager call), then exactly the unlink and publication traversals."""
    ts = [r['t'] for r in rows]
    bad = _contiguous(ts)
    if ts[0] != 0:
        bad.append(f'first row is t={ts[0]}, not the construction traversal 0')
    if not rows[0]['manager_ran'] or rows[0]['phase_in'] != 0:
        bad.append('row 0 does not run the manager in phase 0')
    ran = [r['t'] for r in rows if r['manager_ran']]
    if ran != list(range(ts[0], ts[0] + len(ran))):
        bad.append('manager calls are not one contiguous run from t = 0')
    death = [r['t'] for r in rows if r['manager_ran'] and r['phase_in'] == -3]
    if len(death) != 1:
        bad.append(f'expected exactly one death call (phase -3), found {len(death)}')
    elif ran and death[0] != ran[-1]:
        bad.append(f'manager calls continue after the death call t={death[0]}')
    elif len(rows) != len(ran) + 2:
        bad.append(f'expected exactly two manager-less rows (unlink, publication) after the death, found {len(rows) - len(ran)}')
    for r in rows:
        for k in ('phase_in', 'phase_out', 'length_out', 'selection_out', 'yes_no_out', 'pressed', 'repeat'):
            if (r[k] is None) == r['manager_ran'] or (r[k] is not None and not is_int(r[k])):
                bad.append(f't={r["t"]}: {k} must be an integer exactly when the manager ran')
                break
        if (r['name_out'] is None) == r['manager_ran']:
            bad.append(f't={r["t"]}: name_out must be present exactly when the manager ran')
    return bad

def chapter_coverage(rows):
    ts = [r['t'] for r in rows]
    bad = _contiguous(ts)
    counters = [r['counter_out'] for r in rows]
    if counters != list(range(1, len(rows) + 1)):
        bad.append('chapter counters are not 1..N in order')
    if sum(1 for c in counters if c == 240) != 1:
        bad.append('expected exactly one counter-240 call')
    cleared = [i for i, r in enumerate(rows) if r['command_out'] == [0, 0, 0]]
    if cleared != [len(rows) - 1]:
        bad.append(f'the command must be cleared in the last chapter call only (cleared at {cleared})')
    if any(r['phase_out'] != 19 for r in rows):
        bad.append('chapter phase other than 19')
    return bad

def name_anchors(rows):
    death = next((r['t'] for r in rows if r['manager_ran'] and r['phase_in'] == -3), None)
    return {'presses': [[r['t'], r['pressed']] for r in rows if r['manager_ran'] and r['pressed']], 'death_t': death,
            'traversals': len(rows)}

def chapter_anchors(rows):
    return {'first_t': rows[0]['t'], 'counter_240_t': next((r['t'] for r in rows if r['counter_out'] == 240), None),
            'clear_t': rows[-1]['t'], 'calls': len(rows)}

def compare_rows(orig, nat, what, out):
    first = next((i for i in range(max(len(orig), len(nat)))
                  if i >= len(orig) or i >= len(nat) or canon(orig[i]) != canon(nat[i])), None)
    if first is None:
        print(f'  {what}: identical ({len(orig)} rows)', file=out)
    else:
        print(f'  {what}: DIFFERENT at row {first} ({len(orig)} original / {len(nat)} native): original '
              f'{orig[first] if first < len(orig) else "<absent>"} | native {nat[first] if first < len(nat) else "<absent>"}',
              file=out)
    return first

# ---- identities, pins ------------------------------------------------------------------------------
def verify_original(orig_dir, manifest):
    for cp in CHECKPOINTS:
        for name in ORIGINAL_FILES:
            g.verify_pin(orig_dir, f'{cp}/{name}', manifest)
    for name in ('name-frames.jsonl', 'chapter-frames.jsonl', 'reference.json'):
        g.verify_pin(orig_dir, name, manifest)

def load_route(nat_dir, elf):
    r = parse_json(read_text(Path(nat_dir) / 'route.json'), 'route.json')
    if not isinstance(r, dict) or set(r) != {'route', 'elf_sha256', 'construction_frame', 'checkpoints', 'tails'}:
        raise Malformed('route.json must hold exactly route, elf_sha256, construction_frame, checkpoints, tails')
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
    if not is_int(r['construction_frame']) or r['construction_frame'] != c['name-constructor-return']:
        raise Malformed('route.json: construction_frame must be the name-constructor-return frame')
    if not isinstance(r['tails'], dict) or set(r['tails']) != {'theme_end_t'} or \
            not (r['tails']['theme_end_t'] is None or is_int(r['tails']['theme_end_t'])):
        raise Malformed('route.json: tails must hold exactly theme_end_t (integer or null)')
    return r

def refuse_self_compare(orig_dir, nat_dir):
    o, n = Path(orig_dir).resolve(), Path(nat_dir).resolve()
    if o == n or o in n.parents or n in o.parents:
        raise Malformed('refusing to compare the original analysis with itself (same or nested directory)')
    originals = [p for p in o.rglob('*') if p.is_file()]
    natives = [n / cp / nn for cp in CHECKPOINTS for nn in NATIVE_FILES] + [n / 'name-frames.jsonl', n / 'chapter-frames.jsonl']
    for p in natives:
        if not p.exists():
            continue
        for q in originals:
            if os.path.samefile(p, q):
                raise Malformed(f'refusing self-comparison: native {p.relative_to(n)} is the original {q.relative_to(o)}')

def run(orig_dir, nat_dir, out=None, manifest=MANIFEST, strict=False):
    out = out or sys.stdout
    refuse_self_compare(orig_dir, nat_dir)
    m, elf = g.load_manifest(manifest)
    route = load_route(nat_dir, elf)
    verify_original(orig_dir, m)
    digest = m['result_digest']
    if not isinstance(digest.get('checkpoints'), dict) or not isinstance(digest.get('anchors'), dict):
        raise Malformed('manifest result digest lacks checkpoints/anchors')
    ref = parse_json(read_text(Path(orig_dir) / 'reference.json'), 'reference.json')
    if ref.get('problems') != [] or ref.get('retire_neutrality', {}).get('failures') != [] \
            or ref.get('control_minimal', {}).get('failures') != []:
        raise Malformed('reference.json reports problems, Retire-neutrality or control failures (or no control)')
    if not str(ref.get('rng', {}).get('provenance', '')).startswith('every draw is a hooked'):
        raise Malformed('reference RNG rows are not all hooked observations')
    loaded = {}
    for cp in CHECKPOINTS:
        od, nd = Path(orig_dir) / cp, Path(nat_dir) / cp
        if not nd.is_dir():
            raise Malformed(f'native checkpoint directory {cp} missing')
        if not (nd / 'vram.invalidated').exists():
            raise Malformed(f'native {cp}/vram.invalidated missing (required: draw-target unknownness is compared)')
        o_ev, _ = load_lifecycle(od / 'events.jsonl')
        n_ev, n_as = load_lifecycle(nd / 'lifecycle.jsonl', native=True)
        o_rng, _ = load_rng(od / 'rng.jsonl')
        n_rng, n_ras = load_rng(nd / 'rng.jsonl', native=True)
        loaded[cp] = {'o_ev': o_ev, 'n_ev': n_ev, 'n_as': n_as,
                      'o_st': load_state(od / 'state.json', cp), 'n_st': load_state(nd / 'state.json', cp),
                      'o_fx': base.load_effects(od / 'effects.json'), 'n_fx': base.load_effects(nd / 'effects.json'),
                      'o_rng': o_rng, 'n_rng': n_rng, 'n_ras': n_ras,
                      'o_se': load_session(od / 'session.json'), 'n_se': load_session(nd / 'session.json')}
        d = digest['checkpoints'].get(cp)
        if not isinstance(d, dict) or d.get('resource_events') != sum(1 for e in o_ev if e['kind'] != 'phase') \
                or d.get('rng_draws') != len(o_rng) or d.get('t') != ref['checkpoints'][cp]['t']:
            raise Malformed(f'manifest result digest does not match the original {cp}')
    o_nf, _ = load_frames(Path(orig_dir) / 'name-frames.jsonl', NAME_KEYS, False, 'name frames')
    o_cf, _ = load_frames(Path(orig_dir) / 'chapter-frames.jsonl', CHAPTER_KEYS, False, 'chapter frames')
    if name_coverage(o_nf) or chapter_coverage(o_cf):
        raise Malformed(f'original frames are incomplete: {name_coverage(o_nf) + chapter_coverage(o_cf)}')
    o_anchors = {'checkpoint_t': {cp: ref['checkpoints'][cp]['t'] for cp in CHECKPOINTS}, 'name': name_anchors(o_nf),
                 'chapter': chapter_anchors(o_cf), 'theme_end_t': ref['tails']['theme_end_t']}
    if canon(digest['anchors']) != canon(o_anchors):
        raise Malformed('manifest result digest does not match the original anchors')
    for f in ('name-frames.jsonl', 'chapter-frames.jsonl'):
        if not (Path(nat_dir) / f).exists():
            raise Malformed(f'native {f} missing')
    n_nf, n_nas = load_frames(Path(nat_dir) / 'name-frames.jsonl', NAME_KEYS, True, 'name frames')
    n_cf, n_cas = load_frames(Path(nat_dir) / 'chapter-frames.jsonl', CHAPTER_KEYS, True, 'chapter frames')

    print('New Game -> name entry -> Chapter 1, original vs native:', file=out)
    result = {'route': ROUTE, 'native_frames': route['checkpoints'], 'checkpoints': {}}
    payload_bad = False
    asserted_total = {'lifecycle': 0, 'rng': 0, 'frames': sum(n_nas) + sum(n_cas)}
    prev = None
    s0 = route['construction_frame']
    for cp in CHECKPOINTS:
        L = loaded[cp]
        print(f'  [{cp}] native frame {route["checkpoints"][cp]} (t={route["checkpoints"][cp] - s0}; original '
              f't={ref["checkpoints"][cp]["t"]})', file=out)
        e, first, ea = g.compare_events(L['o_ev'], L['n_ev'], L['n_as'], out)
        s = base.compare_state(L['o_st'], L['n_st'], out)
        fx = int(canon(L['o_fx']) != canon(L['n_fx']))
        print(f'    effect106: DIFFERENT original {L["o_fx"]} | native {L["n_fx"]}' if fx
              else f'    effect106: identical {L["o_fx"]}', file=out)
        r, rfirst, ra = g.compare_rng(L['o_rng'], L['n_rng'], L['n_ras'], out)
        se = compare_session(L['o_se'], L['n_se'], out)
        v, vcounts, vfirst = g.compare_vram(Path(orig_dir) / cp, Path(nat_dir) / cp, out)
        d = digest['checkpoints'][cp]
        if vcounts['required_known'] != d.get('vram_known_nibbles') or vcounts['required_unknown'] != d.get('vram_unknown_nibbles'):
            raise Malformed(f'original VRAM masks of {cp} do not carry the manifest nibble counts')
        inconsistent = []
        if prev is not None:
            pe = [x for x in loaded[prev]['n_ev'] if x['kind'] != 'phase']
            ne = [x for x in L['n_ev'] if x['kind'] != 'phase']
            if [canon(x) for x in ne[:len(pe)]] != [canon(x) for x in pe] or len(ne) < len(pe):
                inconsistent.append('lifecycle')
            if L['n_rng'][:len(loaded[prev]['n_rng'])] != loaded[prev]['n_rng']:
                inconsistent.append('rng')
        if inconsistent:
            print(f'    stream_inconsistent: native {inconsistent} do not extend checkpoint {prev}', file=out)
        asserted_total['lifecycle'] += ea
        asserted_total['rng'] += ra
        bad = bool(e or s or fx or r or se or v or inconsistent)
        payload_bad |= bad
        result['checkpoints'][cp] = {'events_identical': not e, 'first_event_difference': first, 'state_differences': s,
                                     'effect106_identical': not fx, 'rng_identical': not r, 'first_rng_difference': rfirst,
                                     'session_differences': se, 'vram': vcounts, 'vram_first': vfirst,
                                     'stream_inconsistent': inconsistent, 'asserted_events': ea, 'asserted_draws': ra,
                                     'identical': not bad}
        prev = cp
    cov = {'name': name_coverage(n_nf), 'chapter': chapter_coverage(n_cf)}
    incomplete = any(cov.values())
    for k, v in cov.items():
        if v:
            print(f'  {k} frames: INCOMPLETE native coverage: {v}', file=out)
    nfirst = compare_rows(o_nf, n_nf, 'name frames', out)
    cfirst = compare_rows(o_cf, n_cf, 'chapter frames', out)
    n_anchors = {'checkpoint_t': {cp: route['checkpoints'][cp] - s0 for cp in CHECKPOINTS}, 'name': name_anchors(n_nf),
                 'chapter': chapter_anchors(n_cf), 'theme_end_t': route['tails']['theme_end_t']}
    anchors_differ = canon(o_anchors) != canon(n_anchors)
    print(f'  anchors: {"DIFFERENT" if anchors_differ else "identical"} (original {json.dumps(o_anchors)[:300]}...)', file=out)
    if anchors_differ:
        for k in o_anchors:
            if canon(o_anchors[k]) != canon(n_anchors[k]):
                print(f'    anchor {k}: original {o_anchors[k]} | native {n_anchors[k]}', file=out)
    gaps = sum(c['vram']['coverage_gap_invalidated'] for c in result['checkpoints'].values())
    if strict and gaps:
        print(f'  strict: {gaps} coverage gaps are not accepted', file=out)
        payload_bad = True
    n_assert = sum(asserted_total.values())
    print(f'  provenance: owner-asserted rows lifecycle={asserted_total["lifecycle"]} rng={asserted_total["rng"]} '
          f'frames={asserted_total["frames"]} (compared, reported separately, never observation evidence); original draws '
          f'are hooked observations ({ref["rng"]["provenance"]})', file=out)
    print(f'  not compared: phase rows, native frame numbers vs original vblanks; coverage gaps: {gaps}', file=out)
    per_frame_bad = nfirst is not None or cfirst is not None
    if incomplete:
        label, code = 'INCOMPLETE', 5
    elif anchors_differ:
        label, code = 'ANCHORS-DIFFER', 3
        print('  timing anchors differ: the comparison is INCONCLUSIVE; payload differences above are listed, not '
              'attributed to timing, and nothing here is an equality', file=out)
    elif payload_bad or per_frame_bad:
        label, code = 'DIFFERENT', 1
    elif n_assert:
        label, code = 'UNPROVEN', 4
        print('  every compared row matches, but owner-asserted rows were needed: not observation evidence', file=out)
    else:
        label, code = 'IDENTICAL', 0
    result.update({'name_frames_first_difference': nfirst, 'chapter_frames_first_difference': cfirst,
                   'frames_coverage': cov, 'anchors_original': o_anchors, 'anchors_native': n_anchors,
                   'anchors_differ': anchors_differ, 'asserted': asserted_total, 'coverage_gaps': gaps, 'result': label})
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
