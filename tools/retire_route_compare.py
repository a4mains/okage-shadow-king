#!/usr/bin/env python3
"""Fixed-frame comparison of two native route runs (Retire; complete coverage since Restore).

usage: retire_route_compare.py RUN_A RUN_B --route restore|newgame|restore-cancel [--json OUT]

Each run directory holds command.txt, stdout.txt, exit.txt and the --capture sidecars (f.png,
f.png.*.bin, f.png.audio.pcm). Nothing is aligned, shifted or filled.

Expected coverage comes from each run's OWN source events (stdout 'Native ... frame=N'), never from
the other run's records. native_runtime writes a stream record only while its owner is live:
  startup.bin       from 'Native startup constructed selector=S' through the next
                    'Native startup requested selector=...' (the resource retires the next traversal)
  title-state.bin   from 'Native title constructed' to the frame before 'Native title resource/task destroyed'
  restore-state.bin from 'Native Restore constructed' to the frame before 'Native Restore retired'
  name-entry-state.bin  from 'Native name entry constructed' to the frame before 'Native name entry retired'
  game-mode.bin     every frame 1..N (no field mode is reached on these routes)
An interval with no closing event runs to the final frame. Frames outside every interval are
legitimate inactive gaps (transitions) and must have no record. Each --route requires its ordered
construction/retirement events. A missing expected record, or a record in an inactive gap, on either
side makes the result INCOMPLETE (exit 4), listed per stream and side; payload differences are still
reported. startup.bin word 9 (storage[3], the asynchronous host storage probe) is excluded and reported.

Compared: the five per-frame record streams above, the audio PCM and the final PNG. Other capture sidecars
(sound/music/variables/transition/save-state, ...) are NOT compared.
Verdicts: IDENTICAL (0): anchors equal, coverage complete, those five streams, the audio and the final PNG
equal. DIFFERENT (1): anchors equal and something differs. ANCHORS-DIFFER (3): the runs' source-event
frames differ, so a fixed-frame comparison is INCONCLUSIVE; the payload differences are listed but are
NOT attributed to timing (the Retire label TIMING-DIFFERENT overclaimed this). INCOMPLETE (4). 2: malformed.
"""

import json
import re
import sys
from array import array
from pathlib import Path

ROUTES = {
    "restore": ["Native startup constructed selector=3", "Native startup constructed selector=0",
                "Native startup requested selector=1", "Native title constructed",
                "Native title resource/task destroyed", "Native Restore constructed"],
    "newgame": ["Native startup constructed selector=3", "Native startup constructed selector=0",
                "Native startup requested selector=1", "Native title constructed",
                "Native title resource/task destroyed", "Native name entry constructed"],
    "restore-cancel": ["Native startup constructed selector=3", "Native startup constructed selector=0",
                       "Native startup requested selector=1", "Native title constructed",
                       "Native title resource/task destroyed", "Native Restore constructed",
                       "Native Restore retired", "Native title constructed"],
}
INTERVALS = {
    "startup.bin": ("Native startup constructed selector=", "Native startup requested selector=", True),
    "title-state.bin": ("Native title constructed", "Native title resource/task destroyed", False),
    "restore-state.bin": ("Native Restore constructed", "Native Restore retired", False),
    "name-entry-state.bin": ("Native name entry constructed", "Native name entry retired", False),
}
STREAMS = {
    "startup.bin": 12, "title-state.bin": None, "restore-state.bin": None, "name-entry-state.bin": None,
    "game-mode.bin": None,
}
EXCLUDED_WORDS = {"startup.bin": (9,)}
EVENT = re.compile(r"^(Native [^\n]*?) frame=(\d+)$", re.M)

class Malformed(Exception):
    pass

def anchors(run):
    text = (Path(run) / "stdout.txt").read_text()
    return [(m.group(1), int(m.group(2))) for m in EVENT.finditer(text)]

def frames_of(run):
    m = re.search(r"--frames (\d+)", (Path(run) / "command.txt").read_text())
    if not m:
        raise Malformed(f"{run}: command.txt has no --frames")
    return int(m.group(1))

def stream_records(path, length, n):
    """Records keyed by raw frame. An unknown length is the smallest stride whose frame words are all
    in 1..n and strictly increasing (works for short or gapped intervals, unlike consecutive-frame probes)."""
    words = array("I")
    words.frombytes(Path(path).read_bytes())
    if not words:
        return {}, length
    if length is None:
        length = next((L for L in range(2, 8192) if not len(words) % L and all(
            1 <= words[k] <= n and (k == 0 or words[k] > words[k - L]) for k in range(0, len(words), L))), None)
        if length is None:
            raise Malformed(f"{path}: cannot determine the record length")
    if len(words) % length:
        raise Malformed(f"{path}: not a whole number of {length}-word records")
    out = {}
    for i in range(0, len(words), length):
        if words[i] in out:
            raise Malformed(f"{path}: duplicate frame {words[i]}")
        out[words[i]] = words[i:i + length]
    return out, length

def ranges(frames):
    out = []
    for f in sorted(frames):
        if out and out[-1][1] == f - 1:
            out[-1][1] = f
        else:
            out.append([f, f])
    return ",".join(str(a) if a == b else f"{a}-{b}" for a, b in out) or "none"

def expected_frames(events, suffix, n):
    """Frames the run's own events say the stream's owner is live (records expected)."""
    if suffix == "game-mode.bin":
        return set(range(1, n + 1))
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

def check_route(events, route, run):
    want, i = ROUTES[route], 0
    for text, _ in events:
        if i < len(want) and text.startswith(want[i]):
            i += 1
    if i < len(want):
        raise Malformed(f"{run}: route {route} event '{want[i]}' not reported in order")

def compare(a, b, route):
    if Path(a).resolve() == Path(b).resolve():
        raise Malformed("self comparison")
    n = frames_of(a)
    if frames_of(b) != n:
        raise Malformed("runs have different --frames")
    for d in (a, b):
        if not (Path(d) / "exit.txt").exists() or (Path(d) / "exit.txt").read_text().strip() != "0":
            raise Malformed(f"{d}: run exit status is missing or not 0")
    if (Path(a) / "command.txt").read_text() == (Path(b) / "command.txt").read_text():
        raise Malformed("identical command.txt (output paths included): a copy of one run, not two runs")
    result = {"frames": n, "anchors_a": anchors(a), "anchors_b": anchors(b), "streams": {}}
    result["anchors_equal"] = result["anchors_a"] == result["anchors_b"]
    if not result["anchors_a"] or not result["anchors_b"]:
        raise Malformed("no source-event anchors reported")
    check_route(result["anchors_a"], route, a)
    check_route(result["anchors_b"], route, b)
    result["route"] = route
    differs = False
    incomplete = []
    for suffix, declared in STREAMS.items():
        length = declared
        pa, pb = Path(a) / f"f.png.{suffix}", Path(b) / f"f.png.{suffix}"
        if not pa.exists() or not pb.exists():
            raise Malformed(f"missing {suffix}")
        if length is None:
            found = []
            for path in (pa, pb):
                try:
                    found.append(stream_records(path, None, n)[1])
                except Malformed:
                    pass
            found = [L for L in found if L is not None]
            length = min(found) if found else None
        ra, la = stream_records(pa, length, n)
        rb, lb = stream_records(pb, length, n)
        if ra and rb and la != lb:
            raise Malformed(f"{suffix}: record lengths differ ({la}/{lb})")
        skip = EXCLUDED_WORDS.get(suffix, ())

        def key(rec):
            return None if rec is None else [v for i, v in enumerate(rec) if i not in skip]
        cov = {}
        for side, recs, ev in (("a", ra, result["anchors_a"]), ("b", rb, result["anchors_b"])):
            want = expected_frames(ev, suffix, n)
            missing, extra = sorted(want - set(recs)), sorted(set(recs) - want)
            cov[side] = {"expected": ranges(want), "missing": ranges(missing), "unexpected": ranges(extra)}
            if missing or extra:
                incomplete.append(f"{suffix} run {side}: missing {ranges(missing)}, unexpected {ranges(extra)}")
        diff = [f for f in sorted(set(ra) | set(rb)) if key(ra.get(f)) != key(rb.get(f))]
        first = diff[0] if diff else None
        result["streams"][suffix] = {"records_a": len(ra), "records_b": len(rb), "frames_a": ranges(ra),
                                     "frames_b": ranges(rb), "first_difference_frame": first,
                                     "last_difference_frame": diff[-1] if diff else None,
                                     "excluded_words": list(skip), "coverage": cov}
        differs |= first is not None
    pa, pb = array("h"), array("h")
    pa.frombytes((Path(a) / "f.png.audio.pcm").read_bytes())
    pb.frombytes((Path(b) / "f.png.audio.pcm").read_bytes())
    if not pa or len(pa) % n or len(pb) % n:
        raise Malformed("audio is empty or not whole frames")
    per = len(pa) // n
    if len(pa) != len(pb):
        raise Malformed("audio streams differ in length")
    first = next((i for i in range(len(pa)) if pa[i] != pb[i]), None)
    last = next((i for i in range(len(pa) - 1, -1, -1) if pa[i] != pb[i]), None)
    result["audio"] = {"samples": len(pa), "samples_per_frame": per,
                       "first_difference_frame": None if first is None else first // per,
                       "last_difference_frame": None if last is None else last // per}
    differs |= first is not None
    png_equal = (Path(a) / "f.png").read_bytes() == (Path(b) / "f.png").read_bytes()
    result["final_png_identical"] = png_equal
    differs |= not png_equal
    result["incomplete"] = incomplete
    result["payload_differs"] = differs
    result["verdict"] = ("INCOMPLETE" if incomplete else "ANCHORS-DIFFER" if not result["anchors_equal"]
                         else "DIFFERENT" if differs else "IDENTICAL")
    return result

def main(argv=None):
    import argparse
    p = argparse.ArgumentParser(description="fixed-frame route comparison with complete coverage")
    p.add_argument("run_a")
    p.add_argument("run_b")
    p.add_argument("--route", required=True, choices=sorted(ROUTES))
    p.add_argument("--json")
    try:
        args = p.parse_args(sys.argv[1:] if argv is None else argv)
    except SystemExit:
        return 2
    try:
        r = compare(args.run_a, args.run_b, args.route)
    except (Malformed, OSError) as exc:
        print(f"malformed: {exc}", file=sys.stderr)
        return 2
    if args.json:
        Path(args.json).write_text(json.dumps(r, indent=1) + "\n")
    print(f"route {r['route']}; anchors {'equal' if r['anchors_equal'] else 'differ'}: " +
          "; ".join(f"{t.replace('Native ', '')}={fa}/{fb}" for (t, fa), (_, fb) in zip(r["anchors_a"], r["anchors_b"])))
    if len(r["anchors_a"]) != len(r["anchors_b"]):
        print(f"anchor counts differ: {len(r['anchors_a'])}/{len(r['anchors_b'])}")
    for s, v in r["streams"].items():
        excl = f" (word {','.join(map(str, v['excluded_words']))} excluded: asynchronous storage probe)" if v["excluded_words"] else ""
        cov = v["coverage"]
        print(f"{s}{excl}: {v['records_a']}/{v['records_b']} records, expected {cov['a']['expected']} / "
              f"{cov['b']['expected']}, " + ("payload identical" if v["first_difference_frame"] is None else
                                              f"payload differences at raw frames {v['first_difference_frame']}..{v['last_difference_frame']}"))
        for side in ("a", "b"):
            if cov[side]["missing"] != "none" or cov[side]["unexpected"] != "none":
                print(f"  INCOMPLETE run {side}: missing {cov[side]['missing']}, records in inactive gaps {cov[side]['unexpected']}")
    au = r["audio"]
    print(f"audio: {au['samples']} samples, " + ("identical" if au["first_difference_frame"] is None
          else f"differences in raw frames {au['first_difference_frame']}..{au['last_difference_frame']}"))
    print(f"final PNG: {'identical' if r['final_png_identical'] else 'different'}")
    if r["verdict"] == "ANCHORS-DIFFER":
        print("note: source-event frames differ, so this fixed-frame comparison is inconclusive; the payload "
              "differences above are not attributed to timing")
    print("result: " + r["verdict"])
    return {"IDENTICAL": 0, "DIFFERENT": 1, "ANCHORS-DIFFER": 3, "INCOMPLETE": 4}[r["verdict"]]

if __name__ == "__main__":
    sys.exit(main())
