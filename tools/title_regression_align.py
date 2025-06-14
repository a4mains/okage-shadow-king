#!/usr/bin/env python3
"""Align two native cold-start runs on source events and compare their per-frame records.

usage: title_regression_align.py RUN_A RUN_B   (directories made by tools/rng_cold_start_regression.sh)

Storage readiness completes asynchronously on a host thread, so the logo and title schedules start at
run-dependent frames. This tool never chooses an anchor: each run supplies its own source-event frames
from its stdout ('Native startup constructed selector=0', 'Native startup requested selector=1',
'Native title constructed'). Event-relative alignment is diagnostic; it is not fixed-frame equality.

Coverage policy (Retire; the Title version skipped frames absent on both sides and accepted half coverage):
  * logo phase: every frame from selector-0 construction through the selector-1 request, inclusive,
    must have a startup.bin record (12 words) on BOTH sides. A missing frame is malformed (exit 2) and
    is listed. Compared without column 0 (frame) and column 9 (storage[3], the asynchronous probe).
  * transition: the frames strictly between the selector-1 request and title construction. native_runtime
    writes a startup record only while the startup resource is live (after GameModeParent::tick). The
    traversal after the request retires the resource (mode-8 destroy_child, 101058), the next frees the
    wrapper, and title construction follows. These frames must have NO startup record on either side;
    a record on either side, or a different transition length, is a difference. They are listed.
  * title phase: every frame from title construction to the final frame must have a title-state
    record on each side (else malformed); the common aligned span is compared (frame column removed)
    and the uncompared tail of the longer side is reported.
  * audio PCM from title construction to the end, common aligned span.
Not compared (reported): frames before selector-0 construction, game-mode.bin, music/sound sidecars,
the final PNG. Exit status: 0 identical, 1 different, 2 malformed or incomplete.
"""
import re
import sys
from array import array
from pathlib import Path

LOGO_EVENT = "Native startup constructed selector=0"
REQUEST_EVENT = "Native startup requested selector=1"
TITLE_EVENT = "Native title constructed"

class Malformed(Exception):
    pass

def event_frame(run, text):
    hits = re.findall(re.escape(text) + r" frame=(\d+)", (Path(run) / "stdout.txt").read_text())
    if not hits:
        raise Malformed(f"{run}: event '{text}' not reported")
    return int(hits[0])

def frames_of(run):
    m = re.search(r"--frames (\d+)", (Path(run) / "command.txt").read_text())
    if not m:
        raise Malformed(f"{run}: command.txt has no --frames")
    return int(m.group(1))

def records(path, length=None):
    words = array("I")
    words.frombytes(Path(path).read_bytes())
    if not words:
        raise Malformed(f"{path}: empty")
    if length is None:
        first = words[0]
        length = next((L for L in range(2, 4096) if all(
            (k * L < len(words) and words[k * L] == first + k) for k in range(5))), None)
        if length is None:
            raise Malformed(f"{path}: cannot determine the record length")
    if len(words) % length:
        raise Malformed(f"{path}: not a whole number of {length}-word records")
    out = {}
    for i in range(0, len(words), length):
        if words[i] in out:
            raise Malformed(f"{path}: duplicate frame {words[i]}")
        out[words[i]] = list(words[i:i + length])
    return out, length

def ranges(frames):
    """Compact [a-b, ...] text for a sorted frame list."""
    out, frames = [], sorted(frames)
    for f in frames:
        if out and out[-1][1] == f - 1:
            out[-1][1] = f
        else:
            out.append([f, f])
    return ",".join(str(a) if a == b else f"{a}-{b}" for a, b in out) or "none"

def require_complete(name, recs, first, last):
    """Every frame in [first, last] must have a record; return nothing or raise listing the gaps."""
    missing = [f for f in range(first, last + 1) if f not in recs]
    if missing:
        raise Malformed(f"{name}: {len(missing)} of {last - first + 1} expected frames have no record: {ranges(missing)}")

def compare_span(name, ra, rb, start_a, start_b, span, skip, out):
    for k in range(span):
        a = [v for i, v in enumerate(ra[start_a + k]) if i not in skip]
        b = [v for i, v in enumerate(rb[start_b + k]) if i not in skip]
        if a != b:
            print(f"{name}: {span} aligned frames, first difference at offset {k} "
                  f"(raw frames {start_a + k}/{start_b + k})", file=out)
            return 1
    print(f"{name}: {span} aligned frames, all {span} recorded on both sides, identical", file=out)
    return 0

def run(a, b, out=None):
    out = out or sys.stdout
    if Path(a).resolve() == Path(b).resolve():
        raise Malformed("self comparison")
    n = frames_of(a)
    if frames_of(b) != n:
        raise Malformed("runs have different --frames")
    ev = {}
    for side, d in (("a", a), ("b", b)):
        l, r, t = (event_frame(d, e) for e in (LOGO_EVENT, REQUEST_EVENT, TITLE_EVENT))
        if not l < r < t <= n:
            raise Malformed(f"{d}: event order {l}/{r}/{t} is not selector-0 < request < title <= {n}")
        ev[side] = (l, r, t)
    (la, ra_, ta), (lb, rb_, tb) = ev["a"], ev["b"]
    print(f"anchors: logos {la}/{lb}, selector-1 request {ra_}/{rb_}, title {ta}/{tb} "
          f"(offsets {lb - la}, {rb_ - ra_}, {tb - ta}); frames {n}", file=out)
    diffs = 0
    sa, _ = records(Path(a) / "f.png.startup.bin", 12)
    sb, _ = records(Path(b) / "f.png.startup.bin", 12)
    require_complete(f"{a} logo phase", sa, la, ra_)
    require_complete(f"{b} logo phase", sb, lb, rb_)
    if ra_ - la != rb_ - lb:
        print(f"logo phase: lengths differ ({ra_ - la + 1}/{rb_ - lb + 1} frames)", file=out)
        diffs += 1
    else:
        diffs += compare_span("logo phase (startup.bin)", sa, sb, la, lb, ra_ - la + 1, {0, 9}, out)
    trans_a, trans_b = range(ra_ + 1, ta), range(rb_ + 1, tb)
    present = [f for f in trans_a if f in sa] + [f for f in trans_b if f in sb]
    if len(trans_a) != len(trans_b) or present:
        print(f"transition: lengths {len(trans_a)}/{len(trans_b)}, startup records present at {ranges(present)}",
              file=out)
        diffs += 1
    else:
        print(f"transition: {len(trans_a)} frames (raw {ranges(trans_a)} / {ranges(trans_b)}) have no startup "
              "record on either side, as required (resource retired, wrapper freed)",
              file=out)
    xa, x_len = records(Path(a) / "f.png.title-state.bin")
    xb, y_len = records(Path(b) / "f.png.title-state.bin")
    if y_len != x_len:
        raise Malformed(f"title record lengths differ ({x_len}/{y_len})")
    for d, recs, t in ((a, xa, ta), (b, xb, tb)):
        require_complete(f"{d} title phase", recs, t, n)
        early = [f for f in recs if f < t or f > n]
        if early:
            raise Malformed(f"{d} title phase: records outside [{t},{n}] at {ranges(early)}")
    span = min(n - ta, n - tb) + 1
    diffs += compare_span("title phase (title-state.bin)", xa, xb, ta, tb, span, {0}, out)
    if ta != tb:
        print(f"title phase: {abs(ta - tb)} trailing frames of the earlier run not compared (alignment offset)",
              file=out)
    pa, pb = array("h"), array("h")
    pa.frombytes((Path(a) / "f.png.audio.pcm").read_bytes())
    pb.frombytes((Path(b) / "f.png.audio.pcm").read_bytes())
    if len(pa) != len(pb) or len(pa) % n or not pa:
        raise Malformed("audio streams are empty, differ in length or are not whole frames")
    per = len(pa) // n
    count = (n - max(ta, tb)) * per
    if count <= 0:
        raise Malformed("audio: no aligned samples after title construction")
    first = next((i for i in range(count) if pa[ta * per + i] != pb[tb * per + i]), None)
    print(f"audio after title construction: {count} aligned samples, " +
          ("identical" if first is None else f"first difference at aligned sample {first}"), file=out)
    diffs += first is not None
    print("not compared: frames before selector-0 construction, game-mode.bin, music/sound sidecars, final PNG",
          file=out)
    print("result: " + ("ALIGNED-IDENTICAL" if not diffs else "DIFFERENT"), file=out)
    return 1 if diffs else 0

def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        return run(*argv)
    except (Malformed, OSError) as exc:
        print(f"malformed: {exc}", file=sys.stderr)
        return 2

if __name__ == "__main__":
    sys.exit(main())
