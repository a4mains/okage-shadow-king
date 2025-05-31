#!/usr/bin/env python3
"""Align two native cold-start runs on a source event and compare per-frame records.

usage: title_regression_align.py RUN_A RUN_B   (directories made by tools/rng_cold_start_regression.sh)

Storage readiness completes asynchronously on a host thread, so the logo/title schedule starts
at run-dependent frames. This tool does not choose an anchor to manufacture equality: it uses the
frame each run itself reports for 'Native title constructed' (stdout) and compares the title-state
records (title_trace, frame column removed) frame-for-frame after that event, and the PCM audio
after the corresponding sample offset. It reports the first difference or identity for each.
"""
import re
import sys
from pathlib import Path
from array import array

def event_frame(run, text):
    m = re.search(text + r" frame=(\d+)", (Path(run) / "stdout.txt").read_text())
    if not m:
        raise SystemExit(f"{run}: event '{text}' not reported")
    return int(m.group(1))

def records(path):
    words = array("I"); words.frombytes(Path(path).read_bytes())
    # record length: distance between consecutive frame numbers in column 0
    first = words[0]
    length = next(i for i in range(1, len(words)) if words[i] == first + 1)
    if len(words) % length:
        raise SystemExit(f"{path}: not a whole number of {length}-word records")
    return {words[i]: list(words[i + 1:i + length]) for i in range(0, len(words), length)}, length

def main(a, b):
    fa, fb = event_frame(a, "Native title constructed"), event_frame(b, "Native title constructed")
    ra, la = records(Path(a) / "f.png.title-state.bin")
    rb, lb = records(Path(b) / "f.png.title-state.bin")
    print(f"title constructed: {a} frame {fa}, {b} frame {fb} (offset {fb - fa}); record words {la}/{lb}")
    common = [k for k in sorted(ra) if k - fa >= 0 and (k - fa + fb) in rb]
    diff = next((k for k in common if ra[k] != rb[k - fa + fb]), None)
    print(f"title-state: {len(common)} aligned frames, " +
          ("identical" if diff is None else f"first difference at {a} frame {diff} ({diff - fa} after construction)"))
    pa = array("h"); pa.frombytes((Path(a) / "f.png.audio.pcm").read_bytes())
    pb = array("h"); pb.frombytes((Path(b) / "f.png.audio.pcm").read_bytes())
    total_frames = 1800
    per_frame = len(pa) // total_frames
    sa, sb = fa * per_frame, fb * per_frame
    n = min(len(pa) - sa, len(pb) - sb)
    first = next((i for i in range(n) if pa[sa + i] != pb[sb + i]), None)
    print(f"audio after construction: {n} aligned samples, " +
          ("identical" if first is None else f"first difference at aligned sample {first}"))
    return 0 if diff is None and first is None else 1

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__); sys.exit(2)
    sys.exit(main(*sys.argv[1:]))
