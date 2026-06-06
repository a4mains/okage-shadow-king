"""Strict fixed-anchor comparison of two native headless runs.

SCOPE: input-journal records (RUN/inputs.jsonl) and headless status rows
(RUN/stdout.log lines starting with {"frame":) only. This is NOT image, audio
or media equivalence, and frames are never shifted or realigned.

Unlike the historical fixed_anchor_compare.py (dicts keyed by frame, which let
a duplicate tick_attempt overwrite its twin, dropped repeated status rows with
setdefault and compared only common keys), every stream here is an ordered
list that keeps every record, its multiplicity and its position. As in
tools/field_world_compare.py (R2), the record structure is validated before
anything is grouped by frame.

For each side and each stream an explicit inclusive frame interval is selected
(default: that stream's own full range). Then:

* accounting (per side): every frame of the interval has exactly one
  tick_attempt, each attempt is resolved by its tick_complete (or by an error
  record naming it as a failed tick), records never go backwards in frame,
  pressed == held & ~previous held, the stream does not end before the
  interval, and the run's own journaled requests account for it: prelude held
  masks come from RUN/inputs/{buttons,directions}.txt, later held masks from
  the `step` requests in RUN/forwarded-requests.jsonl, and the status rows
  equal (in order and multiplicity) the frames at which `status` was sent;
* comparison (A vs B): positional, record by record, reporting the first
  divergence (index, frame, both records), a tail present on one side only, and
  per-frame record lists that are missing on one side or differ.

Exit 0 only when both requested intervals are fully accounted for on both
sides and identical; 1 when they differ or are not accounted for; 2 when an
input is missing or malformed.
"""
import argparse
import bisect
import collections
import hashlib
import json
import re
import sys
from pathlib import Path

SCOPE = ('Compares input-journal records (inputs.jsonl) and headless status rows (stdout.log) only. '
         'This is NOT image, audio or media equivalence. Frames are never shifted or realigned.')
EXAMPLES = 20
READY = re.compile(r'^Headless ready frame=(\d+)( |$)')

class Malformed(Exception):
    pass

class Rec:
    __slots__ = ('index', 'line', 'kind', 'frame', 'obj', 'canon')

    def __init__(self, index, line, kind, frame, obj):
        self.index, self.line, self.kind, self.frame, self.obj = index, line, kind, frame, obj
        self.canon = json.dumps(obj, sort_keys=True, separators=(',', ':'))

    def show(self):
        return {'index': self.index, 'source_line': self.line, 'frame': self.frame, 'record': self.obj}

def _int(v):
    return isinstance(v, int) and not isinstance(v, bool)

def parse_interval(text):
    try:
        lo, hi = (int(x) for x in text.split(':'))
    except ValueError:
        raise argparse.ArgumentTypeError('interval must be LO:HI')
    if lo > hi:
        raise argparse.ArgumentTypeError('interval LO must be <= HI')
    return (lo, hi)

def load_inputs(run):
    path = Path(run) / 'inputs.jsonl'
    if not path.is_file():
        raise Malformed(f'{path} missing')
    header, records = None, []
    for n, line in enumerate(path.read_text(errors='strict').splitlines(), 1):
        try:
            obj = json.loads(line)
        except ValueError:
            raise Malformed(f'{path}:{n}: malformed JSON')
        if not isinstance(obj, dict) or not isinstance(obj.get('kind'), str):
            raise Malformed(f'{path}:{n}: record without kind')
        if obj['kind'] == 'header':
            if n != 1 or header is not None:
                raise Malformed(f'{path}:{n}: header is not the unique first record')
            header = obj
            continue
        if not _int(obj.get('frame')):
            raise Malformed(f'{path}:{n}: {obj["kind"]} record without integer frame')
        if obj['kind'] == 'tick_attempt' and not (_int(obj.get('held')) and _int(obj.get('pressed'))):
            raise Malformed(f'{path}:{n}: tick_attempt without held/pressed')
        records.append(Rec(len(records), n, obj['kind'], obj['frame'], obj))
    if header is None:
        raise Malformed(f'{path}: no header record')
    return header, records

def load_status(run):
    path = Path(run) / 'stdout.log'
    if not path.is_file():
        raise Malformed(f'{path} missing')
    rows, start = [], None
    for n, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
        if start is None and line.startswith('Headless ready frame='):
            m = READY.match(line)
            if not m:
                raise Malformed(f'{path}:{n}: malformed ready line')
            start = int(m.group(1))
        if not line.startswith('{"frame":'):
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            raise Malformed(f'{path}:{n}: malformed status row')
        if not isinstance(obj, dict) or not _int(obj.get('frame')):
            raise Malformed(f'{path}:{n}: status row without integer frame')
        rows.append(Rec(len(rows), n, 'status', obj['frame'], obj))
    return rows, start

def _prelude_tape(path):
    out = []
    for n, raw in enumerate(Path(path).read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split()
        try:
            out.append((int(parts[0]), int(parts[1], 16)))
        except (IndexError, ValueError):
            raise Malformed(f'{path}:{n}: expected FRAME HEXMASK')
        if len(parts) != 2 or (len(out) > 1 and out[-1][0] <= out[-2][0]):
            raise Malformed(f'{path}:{n}: expected increasing FRAME HEXMASK')
    return out

class Requests:
    """Expected held masks and status frames reconstructed from the run's own
    pinned prelude tapes and journaled requests (never from the other run)."""

    def __init__(self, run, start_frame):
        run = Path(run)
        self.prelude = start_frame
        tapes = []
        for name in ('inputs/buttons.txt', 'inputs/directions.txt'):
            if not (run / name).is_file():
                raise Malformed(f'{run / name} missing')
            tapes.append(_prelude_tape(run / name))
        self.tapes = [([f for f, _ in t], [m for _, m in t]) for t in tapes]
        path = run / 'forwarded-requests.jsonl'
        if not path.is_file():
            raise Malformed(f'{path} missing')
        self.steps = []  # (first_frame, last_frame, mask)
        self.status_frames = []
        frame = start_frame
        for n, line in enumerate(path.read_text().splitlines(), 1):
            try:
                rec = json.loads(line)
                cmd = rec['command']
            except (ValueError, KeyError, TypeError):
                raise Malformed(f'{path}:{n}: malformed request record')
            if rec.get('origin') == 'rejected':
                continue
            parts = cmd.split()
            if parts and parts[0] == 'step':
                try:
                    ticks, mask = int(parts[1]), int(parts[2], 16)
                except (IndexError, ValueError):
                    raise Malformed(f'{path}:{n}: malformed step')
                self.steps.append((frame + 1, frame + ticks, mask))
                frame += ticks
            elif cmd == 'status':
                self.status_frames.append(frame)
        self.last_requested = frame
        self.step_starts = [s[0] for s in self.steps]

    def held(self, frame):
        if frame <= self.prelude:
            value = 0
            for frames, masks in self.tapes:
                i = bisect.bisect_right(frames, frame) - 1
                value |= masks[i] if i >= 0 else 0
            return value
        i = bisect.bisect_right(self.step_starts, frame) - 1
        if i >= 0 and self.steps[i][0] <= frame <= self.steps[i][1]:
            return self.steps[i][2]
        return None

def _ranges(frames):
    out = []
    for f in frames:
        if out and f == out[-1][1] + 1:
            out[-1][1] = f
        else:
            out.append([f, f])
    return out

def _cap(items):
    items = list(items)
    return {'count': len(items), 'examples': items[:EXAMPLES]}

def full_range(records):
    return (min(r.frame for r in records), max(r.frame for r in records)) if records else None

def account_inputs(records, interval, requests):
    lo, hi = interval
    sel = [r for r in records if lo <= r.frame <= hi]
    order = [(r.index, prev.frame, r.frame) for prev, r in zip(records, records[1:]) if r.frame < prev.frame
             and lo <= r.frame <= hi]
    attempts = collections.Counter(r.frame for r in sel if r.kind == 'tick_attempt')
    completes = collections.Counter(r.frame for r in sel if r.kind == 'tick_complete')
    missing = [f for f in range(lo, hi + 1) if attempts[f] == 0]
    unresolved, orphan, failed, interleaved = [], [], [], []
    pending = None
    for r in sel:
        if r.kind == 'tick_attempt':
            if pending is not None:
                unresolved.append(pending.frame)
            pending = r
        elif r.kind == 'tick_complete':
            if pending is not None and pending.frame == r.frame:
                pending = None
            else:
                orphan.append(r.frame)
        elif (r.kind == 'error' and pending is not None and r.obj.get('tick_status') == 'failed'
              and r.obj.get('attempted_tick_frame') == pending.frame):
            failed.append(pending.frame)
            pending = None
        elif pending is not None:
            interleaved.append((r.kind, r.frame))
    if pending is not None:
        unresolved.append(pending.frame)
    pressed_bad, held_bad, unrequested = [], [], []
    prev = None
    for r in records:
        if r.kind != 'tick_attempt':
            continue
        if lo <= r.frame <= hi:
            if prev is not None and r.obj['pressed'] != (r.obj['held'] & ~prev & 0xffff):
                pressed_bad.append(r.frame)
            expected = requests.held(r.frame)
            if expected is None:
                unrequested.append(r.frame)
            elif expected != r.obj['held']:
                held_bad.append({'frame': r.frame, 'journal': r.obj['held'], 'requested': expected})
        prev = r.obj['held']
    last = max((r.frame for r in records if r.kind == 'tick_attempt'), default=None)
    tail = None if last is not None and last >= hi else {'stream_last_attempt_frame': last, 'interval_hi': hi}
    problems = {
        'missing_attempt_frames': _cap(_ranges(missing)),
        'duplicate_attempt_frames': _cap(sorted(f for f, n in attempts.items() if n > 1)),
        'duplicate_complete_frames': _cap(sorted(f for f, n in completes.items() if n > 1)),
        'unresolved_attempts': _cap(unresolved),
        'completions_without_attempt': _cap(orphan),
        'records_between_attempt_and_completion': _cap(interleaved),
        'frame_order_violations': _cap(order),
        'pressed_inconsistent_with_held': _cap(pressed_bad),
        'held_differs_from_requests': _cap(held_bad),
        'attempts_without_request': _cap(unrequested),
    }
    failures = [k for k, v in problems.items() if v['count']]
    if tail:
        failures.append('uncaptured_tail')
    return sel, {
        'interval': [lo, hi], 'selected_records': len(sel), 'outside_interval': len(records) - len(sel),
        'kinds': dict(collections.Counter(r.kind for r in sel)),
        'attempts': sum(attempts.values()), 'completions': sum(completes.values()),
        'failed_attempts': failed, 'uncaptured_tail': tail,
        'requested_through_frame': requests.last_requested, **problems,
        'fully_accounted': not failures, 'failures': failures}

def account_status(rows, interval, requests):
    lo, hi = interval
    sel = [r for r in rows if lo <= r.frame <= hi]
    got = [r.frame for r in sel]
    expected = [f for f in requests.status_frames if lo <= f <= hi]
    order = [(r.index, prev.frame, r.frame) for prev, r in zip(rows, rows[1:]) if r.frame < prev.frame]
    first = None
    if got != expected:
        i = next((i for i, (a, b) in enumerate(zip(got, expected)) if a != b), min(len(got), len(expected)))
        first = {'index': i, 'row_frame': got[i] if i < len(got) else None,
                 'requested_frame': expected[i] if i < len(expected) else None}
    g, e = collections.Counter(got), collections.Counter(expected)
    missing = sorted((e - g).elements())
    extra = sorted((g - e).elements())
    last = rows[-1].frame if rows else None
    tail = None if last is not None and last >= max(expected, default=lo) else {
        'stream_last_status_frame': last, 'last_requested_status_frame': max(expected, default=None)}
    failures = []
    if order:
        failures.append('frame_order_violations')
    if got != expected:
        failures.append('status_rows_differ_from_status_requests')
    if tail:
        failures.append('uncaptured_tail')
    return sel, {'interval': [lo, hi], 'selected_rows': len(sel), 'outside_interval': len(rows) - len(sel),
                 'requested_status_rows': len(expected), 'repeated_frames': sum(n - 1 for n in g.values() if n > 1),
                 'rows_missing_vs_requests': _cap(missing), 'rows_extra_vs_requests': _cap(extra),
                 'first_request_mismatch': first, 'frame_order_violations': _cap(order),
                 'uncaptured_tail': tail, 'fully_accounted': not failures, 'failures': failures}

def pairwise(a, b):
    first = None
    for i in range(max(len(a), len(b))):
        ra = a[i] if i < len(a) else None
        rb = b[i] if i < len(b) else None
        if ra is None or rb is None or ra.canon != rb.canon:
            first = {'index': i, 'frame': {'a': ra and ra.frame, 'b': rb and rb.frame},
                     'a': ra and ra.show(), 'b': rb and rb.show()}
            break
    tail = None
    if first is not None and (first['a'] is None or first['b'] is None):
        short, long_ = ('a', b) if first['a'] is None else ('b', a)
        rest = long_[first['index']:]
        tail = {'ends_early': short, 'common_records': first['index'],
                'extra_records_on_other_side': len(rest), 'extra_frames': [rest[0].frame, rest[-1].frame]}
    ga, gb = collections.defaultdict(list), collections.defaultdict(list)
    for r in a:
        ga[r.frame].append(r.canon)
    for r in b:
        gb[r.frame].append(r.canon)
    frames = sorted(set(ga) | set(gb))
    miss_a = [f for f in frames if f not in ga]
    miss_b = [f for f in frames if f not in gb]
    differ = [f for f in frames if f in ga and f in gb and ga[f] != gb[f]]
    mult = [{'frame': f, 'a': len(ga[f]), 'b': len(gb[f])} for f in differ if len(ga[f]) != len(gb[f])]
    return {'records': {'a': len(a), 'b': len(b)},
            'last_frame': {'a': a[-1].frame if a else None, 'b': b[-1].frame if b else None},
            'identical': first is None, 'first_divergence': first,
            'tail_on_one_side': tail,
            'frames_missing_in_a': _cap(_ranges(miss_a)), 'frames_missing_in_b': _cap(_ranges(miss_b)),
            'frames_with_differing_records': _cap(differ), 'multiplicity_differences': _cap(mult)}

def _provenance(run):
    out = {'run': str(Path(run).resolve())}
    m = Path(run) / 'manifest.json'
    if m.is_file():
        out['manifest_sha256'] = hashlib.sha256(m.read_bytes()).hexdigest()
        try:
            man = json.loads(m.read_text())
            out['binary_sha256'] = (man.get('artifacts') or {}).get('okage-native')
            out['prelude_ticks'] = man.get('prelude_ticks')
        except ValueError:
            raise Malformed(f'{m}: malformed manifest')
    return out

def compare(a, b, intervals=None):
    """intervals: {'a_inputs': (lo,hi)|None, 'b_inputs':..., 'a_status':..., 'b_status':...}."""
    intervals = dict(intervals or {})
    report = {'scope': SCOPE, 'a': _provenance(a), 'b': _provenance(b)}
    failures = []
    sides = {}
    for side, run in (('a', a), ('b', b)):
        header, records = load_inputs(run)
        rows, start = load_status(run)
        if start is None:
            raise Malformed(f'{run}: no Headless ready line (start frame unknown)')
        prelude = report[side].get('prelude_ticks')
        if prelude is not None and prelude != start:
            raise Malformed(f'{run}: first ready frame {start} != manifest prelude_ticks {prelude}')
        sides[side] = (header, records, rows, Requests(run, start))
    out_inputs, out_status = {}, {}
    sel_inputs, sel_status = {}, {}
    for side in ('a', 'b'):
        header, records, rows, req = sides[side]
        iv = intervals.get(side + '_inputs') or full_range(records)
        if iv is None:
            raise Malformed(f'{side}: no input records')
        sel_inputs[side], out_inputs[side] = account_inputs(records, iv, req)
        sv = intervals.get(side + '_status') or full_range(rows)
        if sv is None:
            raise Malformed(f'{side}: no status rows')
        sel_status[side], out_status[side] = account_status(rows, sv, req)
        for name, acc in (('inputs', out_inputs[side]), ('status', out_status[side])):
            failures += [f'{side}.{name}.{f}' for f in acc['failures']]
    out_inputs['header_equal'] = sides['a'][0] == sides['b'][0]
    if not out_inputs['header_equal']:
        failures.append('inputs.header_differs')
    out_inputs['comparison'] = pairwise(sel_inputs['a'], sel_inputs['b'])
    out_status['comparison'] = pairwise(sel_status['a'], sel_status['b'])
    for name, out in (('inputs', out_inputs), ('status', out_status)):
        if not out['comparison']['identical']:
            failures.append(name + '.not_identical')
    report['inputs'] = out_inputs
    report['status'] = out_status
    report['failures'] = failures
    report['verdict'] = 'IDENTICAL_AND_FULLY_ACCOUNTED' if not failures else 'DIFFERENT_OR_NOT_ACCOUNTED'
    return report

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--interval', type=parse_interval, help='LO:HI for all four streams')
    for k in ('a_inputs', 'b_inputs', 'a_status', 'b_status'):
        ap.add_argument('--' + k.replace('_', '-'), dest=k, type=parse_interval,
                        help='LO:HI for this stream (default: its own full range)')
    ap.add_argument('--json', help='also write the report here')
    args = ap.parse_args(argv)
    intervals = {k: getattr(args, k) or args.interval for k in ('a_inputs', 'b_inputs', 'a_status', 'b_status')}
    try:
        report = compare(args.a, args.b, intervals)
    except Malformed as e:
        print(json.dumps({'scope': SCOPE, 'verdict': 'MALFORMED', 'error': str(e)}, indent=1))
        return 2
    text = json.dumps(report, indent=1)
    if args.json:
        Path(args.json).write_text(text + '\n')
    print(text)
    print('SCOPE:', SCOPE, file=sys.stderr)
    print('VERDICT:', report['verdict'], file=sys.stderr)
    return 0 if not report['failures'] else 1
