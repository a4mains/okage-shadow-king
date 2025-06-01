#!/usr/bin/env python3
"""Align two native cold-start runs on source events and compare their per-frame records.

usage: title_regression_align.py RUN_A RUN_B   (directories made by tools/rng_cold_start_regression.sh)

Storage readiness completes asynchronously on a host thread, so the logo and title schedules start at
run-dependent frames. This tool never chooses an anchor: each run supplies its own source-event frames
from its stdout ('Native startup constructed selector=0', 'Native title constructed'), and every record
after the event is compared frame-for-frame. Required counts are enforced, so a truncated or empty run
cannot pass.

Compared:
  * logo phase: startup.bin records (12 words) from selector-0 construction up to title construction,
    excluding column 0 (frame) and column 9 (storage[3], the asynchronous probe timing value);
  * title phase: title-state records (frame column removed) from title construction to the end;
  * audio PCM from title construction to the end.
Not compared (reported): everything before selector-0 construction, game-mode.bin, music/sound
sidecars, the final PNG (it shows a different title-relative tick when the anchors differ).
Exit status: 0 identical, 1 different, 2 malformed or incomplete.
"""
import re
import sys
from array import array
from pathlib import Path

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

def compare_phase(name, ra, rb, start_a, start_b, end_a, end_b, skip, out):
    span = min(end_a - start_a, end_b - start_b)
    if span <= 0:
        raise Malformed(f"{name}: empty aligned span")
    present = sum((start_a + k) in ra for k in range(span))
    if present < span // 2:
        raise Malformed(f"{name}: only {present} of {span} aligned frames have records")
    for k in range(span):
        if ((start_a + k) in ra) != ((start_b + k) in rb):
            print(f"{name}: {span} aligned frames, record present on one side only at offset {k}", file=out)
            return 1
        if (start_a + k) not in ra:
            continue
        a = [v for i, v in enumerate(ra[start_a + k]) if i not in skip]
        b = [v for i, v in enumerate(rb[start_b + k]) if i not in skip]
        if a != b:
            print(f"{name}: {span} aligned frames, first difference at offset {k}", file=out)
            return 1
    print(f"{name}: {span} aligned frames, identical", file=out)
    return 0

def run(a, b, out=None):
    out = out or sys.stdout
    n = frames_of(a)
    if frames_of(b) != n:
        raise Malformed("runs have different --frames")
    la, lb = event_frame(a, "Native startup constructed selector=0"), event_frame(b, "Native startup constructed selector=0")
    ta, tb = event_frame(a, "Native title constructed"), event_frame(b, "Native title constructed")
    print(f"anchors: logos {la}/{lb}, title {ta}/{tb} (offsets {lb - la}, {tb - ta}); frames {n}", file=out)
    sa, _ = records(Path(a) / "f.png.startup.bin", 12)
    sb, _ = records(Path(b) / "f.png.startup.bin", 12)
    diffs = compare_phase("logo phase (startup.bin)", sa, sb, la, lb, ta, tb, {0, 9}, out)
    xa, x_len = records(Path(a) / "f.png.title-state.bin")
    xb, y_len = records(Path(b) / "f.png.title-state.bin")
    if y_len != x_len:
        raise Malformed(f"title record lengths differ ({x_len}/{y_len})")
    # title records start the frame after construction (the first title update)
    diffs += compare_phase("title phase (title-state.bin)", xa, xb, ta + 1, tb + 1, n + 1, n + 1, {0}, out)
    pa, pb = array("h"), array("h")
    pa.frombytes((Path(a) / "f.png.audio.pcm").read_bytes())
    pb.frombytes((Path(b) / "f.png.audio.pcm").read_bytes())
    if len(pa) != len(pb) or len(pa) % n:
        raise Malformed("audio streams differ in length or are not whole frames")
    per = len(pa) // n
    count = min(len(pa) - ta * per, len(pb) - tb * per)
    expected = (n - max(ta, tb)) * per
    if count != expected or count <= 0:
        raise Malformed(f"audio: {count} aligned samples, expected {expected}")
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
