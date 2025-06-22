#!/usr/bin/env python3
"""Fixed-frame comparison of two native route runs (Retire). usage: retire_route_compare.py RUN_A RUN_B [--json OUT]

Each run directory holds command.txt, stdout.txt and the --capture sidecars (f.png, f.png.*.bin,
f.png.audio.pcm). Nothing is aligned or shifted: every per-frame record stream is compared at its
raw frame numbers, audio sample-for-sample and the final PNG byte-for-byte. For each stream the
tool reports the recorded frame ranges on both sides and the first raw frame that differs (a
frame recorded on one side only is a difference). Source-event anchors (stdout 'frame=' lines)
are reported; when they differ, divergence is expected from the asynchronous storage probe and
the verdict says TIMING-DIFFERENT instead of claiming a behavioural difference.
Exit: 0 identical, 1 different (anchors equal), 3 timing-different, 2 malformed.
"""
import json
import re
import sys
from array import array
from pathlib import Path

STREAMS = {
    "startup.bin": 12, "title-state.bin": None, "restore-state.bin": None, "name-entry-state.bin": None,
    "game-mode.bin": None,
}
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

def stream_records(path, length):
    words = array("I")
    words.frombytes(Path(path).read_bytes())
    if not words:
        return {}, length
    if length is None:
        first = words[0]
        length = next((L for L in range(2, 8192) if all(
            k * L < len(words) and words[k * L] == first + k for k in range(3))), None)
        if length is None:
            raise Malformed(f"{path}: cannot determine the record length")
    if len(words) % length:
        raise Malformed(f"{path}: not a whole number of {length}-word records")
    out = {}
    for i in range(0, len(words), length):
        if words[i] in out:
            raise Malformed(f"{path}: duplicate frame {words[i]}")
        out[words[i]] = words[i:i + length].tobytes()
    return out, length

def ranges(frames):
    out = []
    for f in sorted(frames):
        if out and out[-1][1] == f - 1:
            out[-1][1] = f
        else:
            out.append([f, f])
    return ",".join(str(a) if a == b else f"{a}-{b}" for a, b in out) or "none"

def compare(a, b):
    if Path(a).resolve() == Path(b).resolve():
        raise Malformed("self comparison")
    n = frames_of(a)
    if frames_of(b) != n:
        raise Malformed("runs have different --frames")
    for d in (a, b):
        if (Path(d) / "exit.txt").exists() and (Path(d) / "exit.txt").read_text().strip() != "0":
            raise Malformed(f"{d}: run exit status is not 0")
    result = {"frames": n, "anchors_a": anchors(a), "anchors_b": anchors(b), "streams": {}}
    result["anchors_equal"] = result["anchors_a"] == result["anchors_b"]
    if not result["anchors_a"]:
        raise Malformed("no source-event anchors reported")
    differs = False
    for suffix, length in STREAMS.items():
        pa, pb = Path(a) / f"f.png.{suffix}", Path(b) / f"f.png.{suffix}"
        if not pa.exists() or not pb.exists():
            raise Malformed(f"missing {suffix}")
        ra, la = stream_records(pa, length)
        rb, lb = stream_records(pb, length)
        if ra and rb and la != lb:
            raise Malformed(f"{suffix}: record lengths differ ({la}/{lb})")
        diff = [f for f in sorted(set(ra) | set(rb)) if ra.get(f) != rb.get(f)]
        first = diff[0] if diff else None
        result["streams"][suffix] = {"records_a": len(ra), "records_b": len(rb), "frames_a": ranges(ra),
                                     "frames_b": ranges(rb), "first_difference_frame": first,
                                     "last_difference_frame": diff[-1] if diff else None}
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
    result["verdict"] = ("TIMING-DIFFERENT" if not result["anchors_equal"] else "DIFFERENT" if differs else "IDENTICAL")
    return result

def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    out = None
    if "--json" in argv:
        i = argv.index("--json")
        out = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        r = compare(*argv)
    except (Malformed, OSError) as exc:
        print(f"malformed: {exc}", file=sys.stderr)
        return 2
    if out:
        Path(out).write_text(json.dumps(r, indent=1) + "\n")
    print(f"anchors {'equal' if r['anchors_equal'] else 'differ'}: " +
          "; ".join(f"{t.replace('Native ', '')}={fa}/{fb}" for (t, fa), (_, fb) in zip(r["anchors_a"], r["anchors_b"])))
    for s, v in r["streams"].items():
        print(f"{s}: {v['records_a']}/{v['records_b']} records, frames {v['frames_a']} / {v['frames_b']}, "
              + ("identical" if v["first_difference_frame"] is None else
               f"differences at raw frames {v['first_difference_frame']}..{v['last_difference_frame']}"))
    au = r["audio"]
    print(f"audio: {au['samples']} samples, " + ("identical" if au["first_difference_frame"] is None
          else f"differences in raw frames {au['first_difference_frame']}..{au['last_difference_frame']}"))
    print(f"final PNG: {'identical' if r['final_png_identical'] else 'different'}")
    print("result: " + r["verdict"])
    return {"IDENTICAL": 0, "DIFFERENT": 1, "TIMING-DIFFERENT": 3}[r["verdict"]]

if __name__ == "__main__":
    sys.exit(main())
