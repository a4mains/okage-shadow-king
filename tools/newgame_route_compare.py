#!/usr/bin/env python3
"""Fixed-frame comparison of two native New Game -> name entry -> Chapter 1 route runs (Newgame; complete coverage).

usage: newgame_route_compare.py RUN_A RUN_B [--json OUT]

Each run directory holds command.txt, stdout.txt, exit.txt and the --capture sidecars (f.png, f.png.*.bin,
f.png.audio.pcm) of an `okage-native` run with the same fixed raw-frame tape. Nothing is aligned, shifted or filled.

Expected coverage comes from each run's OWN source events (stdout 'Native ... frame=N'), never from the other run
(tools/retire_route_compare.py rules, extended):
  startup.bin, title-state.bin, name-entry-state.bin   as Retire (owner-live intervals)
  chapter-state.bin  from 'Native chapter constructed' to the frame before 'Native chapter retired'
  game-mode.bin      records keyed by (frame, point); native writes point 0 before and point 1 after the mode
                     parent's traversal unless the parent is in steady mode 2 (no request, no dead allocation):
                     frames 1..C-1 both points, C point 0, C+1..R-1 none, R point 1, R+1..Q-1 both, Q point 0, Q+1..N
                     none, with C/R/Q = 'Native chapter constructed' / 'Native chapter requests ordinary field' /
                     'Native ordinary field queued' frames. A later mode request would add records: they are reported
                     as unexpected (INCOMPLETE), never silently accepted.
Compared: those five per-frame streams (startup.bin word 9, the asynchronous storage probe, excluded), the audio PCM
and the final PNG. Verdicts as Retire: IDENTICAL (0); DIFFERENT (1) anchors equal and something differs; ANCHORS-DIFFER
(3) the source-event frames differ, so the fixed-frame comparison is INCONCLUSIVE and differences are not attributed;
INCOMPLETE (4); 2 malformed.
"""
import json
import sys
from array import array
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import retire_route_compare as base  # noqa: E402

ROUTE = base.ROUTES['newgame'] + ['Native name entry requests opening field', 'Native name entry retired',
                                  'Native chapter constructed', 'Native chapter requests ordinary field',
                                  'Native chapter retired', 'Native ordinary field queued']
INTERVALS = dict(base.INTERVALS)
INTERVALS['chapter-state.bin'] = ('Native chapter constructed', 'Native chapter retired', False)
STREAMS = {'startup.bin': 12, 'title-state.bin': None, 'name-entry-state.bin': None, 'chapter-state.bin': 6}
GAME_MODE_WORDS = 25
Malformed = base.Malformed

def event_frame(events, prefix):
    hits = [f for t, f in events if t.startswith(prefix)]
    if len(hits) != 1:
        raise Malformed(f"expected exactly one '{prefix}' event, found {len(hits)}")
    return hits[0]

def interval_frames(events, suffix, n):
    begin, end, inclusive = INTERVALS[suffix]
    frames, open_at = set(), None
    for text, f in events:
        if text.startswith(begin) and open_at is None:
            open_at = f
        elif text.startswith(end) and open_at is not None:
            frames |= set(range(open_at, f + 1 if inclusive else f))
            open_at = None
    if open_at is not None:
        frames |= set(range(open_at, n + 1))
    return frames

def game_mode_expected(events, n):
    c = event_frame(events, 'Native chapter constructed')
    r = event_frame(events, 'Native chapter requests ordinary field')
    q = event_frame(events, 'Native ordinary field queued')
    want = set()
    for f in range(1, n + 1):
        if f < c or r < f < q:
            want |= {(f, 0), (f, 1)}
        elif f in (c, q):
            want.add((f, 0))
        elif f == r:
            want.add((f, 1))
    return want

def game_mode_records(path, n):
    words = array('I')
    words.frombytes(Path(path).read_bytes())
    if len(words) % GAME_MODE_WORDS:
        raise Malformed(f'{path}: not a whole number of {GAME_MODE_WORDS}-word records')
    out = {}
    for i in range(0, len(words), GAME_MODE_WORDS):
        key = (words[i], words[i + 1])
        if key in out or not 1 <= key[0] <= n or key[1] not in (0, 1):
            raise Malformed(f'{path}: duplicate or invalid (frame, point) {key}')
        out[key] = words[i:i + GAME_MODE_WORDS]
    return out

def keyranges(keys):
    return ','.join(f'{f}.{p}' for f, p in sorted(keys)[:12]) + ('...' if len(keys) > 12 else '') if keys else 'none'

def compare(a, b):
    if Path(a).resolve() == Path(b).resolve():
        raise Malformed('self comparison')
    n = base.frames_of(a)
    if base.frames_of(b) != n:
        raise Malformed('runs have different --frames')
    for d in (a, b):
        if not (Path(d) / 'exit.txt').exists() or (Path(d) / 'exit.txt').read_text().strip() != '0':
            raise Malformed(f'{d}: run exit status is missing or not 0')
    if (Path(a) / 'command.txt').read_text() == (Path(b) / 'command.txt').read_text():
        raise Malformed('identical command.txt (output paths included): a copy of one run, not two runs')
    result = {'frames': n, 'anchors_a': base.anchors(a), 'anchors_b': base.anchors(b), 'streams': {}}
    result['anchors_equal'] = result['anchors_a'] == result['anchors_b']
    for side, ev, run in (('a', result['anchors_a'], a), ('b', result['anchors_b'], b)):
        want, i = ROUTE, 0
        for text, _ in ev:
            if i < len(want) and text.startswith(want[i]):
                i += 1
        if i < len(want):
            raise Malformed(f"{run}: route event '{want[i]}' not reported in order")
    differs, incomplete = False, []
    for suffix, declared in STREAMS.items():
        pa, pb = Path(a) / f'f.png.{suffix}', Path(b) / f'f.png.{suffix}'
        if not pa.exists() or not pb.exists():
            raise Malformed(f'missing {suffix}')
        length = declared
        if length is None:
            found = []
            for path in (pa, pb):
                try:
                    found.append(base.stream_records(path, None, n)[1])
                except Malformed:
                    pass
            found = [L for L in found if L is not None]
            length = min(found) if found else None
        ra, la = base.stream_records(pa, length, n)
        rb, lb = base.stream_records(pb, length, n)
        if ra and rb and la != lb:
            raise Malformed(f'{suffix}: record lengths differ ({la}/{lb})')
        skip = base.EXCLUDED_WORDS.get(suffix, ())

        def key(rec):
            return None if rec is None else [v for i, v in enumerate(rec) if i not in skip]
        cov = {}
        for side, recs, ev in (('a', ra, result['anchors_a']), ('b', rb, result['anchors_b'])):
            want = interval_frames(ev, suffix, n)
            missing, extra = sorted(want - set(recs)), sorted(set(recs) - want)
            cov[side] = {'expected': base.ranges(want), 'missing': base.ranges(missing), 'unexpected': base.ranges(extra)}
            if missing or extra:
                incomplete.append(f'{suffix} run {side}: missing {base.ranges(missing)}, unexpected {base.ranges(extra)}')
        diff = [f for f in sorted(set(ra) | set(rb)) if key(ra.get(f)) != key(rb.get(f))]
        result['streams'][suffix] = {'records_a': len(ra), 'records_b': len(rb), 'first_difference_frame': diff[0] if diff else None,
                                     'last_difference_frame': diff[-1] if diff else None, 'excluded_words': list(skip),
                                     'coverage': cov}
        differs |= bool(diff)
    ga, gb = game_mode_records(Path(a) / 'f.png.game-mode.bin', n), game_mode_records(Path(b) / 'f.png.game-mode.bin', n)
    cov = {}
    for side, recs, ev in (('a', ga, result['anchors_a']), ('b', gb, result['anchors_b'])):
        want = game_mode_expected(ev, n)
        missing, extra = sorted(want - set(recs)), sorted(set(recs) - want)
        cov[side] = {'expected': len(want), 'missing': keyranges(missing), 'unexpected': keyranges(extra)}
        if missing or extra:
            incomplete.append(f'game-mode.bin run {side}: missing {keyranges(missing)}, unexpected {keyranges(extra)}')
    gdiff = [k for k in sorted(set(ga) | set(gb)) if ga.get(k) != gb.get(k)]
    words = sorted({i for k in gdiff if k in ga and k in gb for i, (x, y) in enumerate(zip(ga[k], gb[k])) if x != y})
    result['streams']['game-mode.bin'] = {'records_a': len(ga), 'records_b': len(gb),
                                          'first_difference': list(gdiff[0]) if gdiff else None,
                                          'last_difference': list(gdiff[-1]) if gdiff else None,
                                          'differing_words': words, 'coverage': cov}
    differs |= bool(gdiff)
    pa, pb = array('h'), array('h')
    pa.frombytes((Path(a) / 'f.png.audio.pcm').read_bytes())
    pb.frombytes((Path(b) / 'f.png.audio.pcm').read_bytes())
    if not pa or len(pa) % n or len(pb) % n or len(pa) != len(pb):
        raise Malformed('audio is empty, not whole frames, or of different lengths')
    per = len(pa) // n
    first = next((i for i in range(len(pa)) if pa[i] != pb[i]), None)
    last = next((i for i in range(len(pa) - 1, -1, -1) if pa[i] != pb[i]), None)
    result['audio'] = {'samples': len(pa), 'samples_per_frame': per,
                       'first_difference_frame': None if first is None else first // per,
                       'last_difference_frame': None if last is None else last // per}
    differs |= first is not None
    result['final_png_identical'] = (Path(a) / 'f.png').read_bytes() == (Path(b) / 'f.png').read_bytes()
    differs |= not result['final_png_identical']
    result['incomplete'] = incomplete
    result['payload_differs'] = differs
    result['verdict'] = ('INCOMPLETE' if incomplete else 'ANCHORS-DIFFER' if not result['anchors_equal']
                         else 'DIFFERENT' if differs else 'IDENTICAL')
    return result

def main(argv=None):
    import argparse
    p = argparse.ArgumentParser(description='fixed-frame New Game route comparison with complete coverage')
    p.add_argument('run_a')
    p.add_argument('run_b')
    p.add_argument('--json')
    args = p.parse_args(argv)
    try:
        r = compare(args.run_a, args.run_b)
    except (Malformed, OSError) as exc:
        print(f'malformed: {exc}', file=sys.stderr)
        return 2
    if args.json:
        Path(args.json).write_text(json.dumps(r, indent=1) + '\n')
    print(f"anchors {'equal' if r['anchors_equal'] else 'differ'}: " +
          '; '.join(f"{t.replace('Native ', '')}={fa}/{fb}" for (t, fa), (_, fb) in zip(r['anchors_a'], r['anchors_b'])))
    for s, v in r['streams'].items():
        d = v.get('first_difference_frame', v.get('first_difference'))
        last = v.get('last_difference_frame', v.get('last_difference'))
        extra = f" (differing words {v['differing_words']})" if v.get('differing_words') else ''
        print(f"{s}: {v['records_a']}/{v['records_b']} records, " +
              ('payload identical' if d is None else f'payload differences {d}..{last}{extra}'))
        for side in ('a', 'b'):
            c = v['coverage'][side]
            if c['missing'] != 'none' or c['unexpected'] != 'none':
                print(f"  INCOMPLETE run {side}: missing {c['missing']}, unexpected {c['unexpected']}")
    au = r['audio']
    print(f"audio: {au['samples']} samples, " + ('identical' if au['first_difference_frame'] is None
          else f"differences in raw frames {au['first_difference_frame']}..{au['last_difference_frame']}"))
    print(f"final PNG: {'identical' if r['final_png_identical'] else 'different'}")
    if r['verdict'] == 'ANCHORS-DIFFER':
        print('note: source-event frames differ, so this fixed-frame comparison is inconclusive; the payload '
              'differences above are not attributed to timing')
    print('result: ' + r['verdict'])
    return {'IDENTICAL': 0, 'DIFFERENT': 1, 'ANCHORS-DIFFER': 3, 'INCOMPLETE': 4}[r['verdict']]

if __name__ == '__main__':
    sys.exit(main())
