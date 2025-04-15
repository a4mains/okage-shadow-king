#!/usr/bin/env python3
"""Compare the ordered startup resource/RNG timeline: unchanged original vs native.

usage: rng_startup_timeline_compare.py ORIGINAL_EVENTS.jsonl NATIVE_REPORT_DIR

ORIGINAL_EVENTS.jsonl is a retained Startup capture (runs/<run>/events.jsonl), verified against the
sha256 pin in docs/startup-original-manifest.json before use. NATIVE_REPORT_DIR is
`okage-native --startup-texture-report` output (timeline.jsonl, state.json).

Both sides are reduced to the same ordered list: every allocation (0x1cf008 return; allocation id)
and every host-to-local upload (0x1c9bb8 entry; destination allocation id), each with the number of
libc rand() draws (0x1f14b0) made before it, and the total draw count at the boundary. This proves
where the noise draws, the 31 effect uploads, the EFD preparation draws and the fonts fall relative to
each other; it is not a comparison of the drawn values (tools/rng_compare.py does that).

Exit status: 0 identical, 1 different, 2 malformed or unpinned input.
"""
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs/startup-original-manifest.json"
EXPECTED = {"alloc": 79, "upload": 72, "draws": 9504}

class Malformed(Exception):
    pass

def strict_loads(text, where):
    def no_dup(pairs):
        out = {}
        for k, v in pairs:
            if k in out:
                raise ValueError(f"duplicate key {k}")
            out[k] = v
        return out
    try:
        return json.loads(text, object_pairs_hook=no_dup,
                          parse_constant=lambda c: (_ for _ in ()).throw(ValueError("non-finite " + c)))
    except ValueError as exc:
        raise Malformed(f"{where}: {exc}")

def pinned(events_path, manifest_path=MANIFEST):
    manifest = strict_loads(Path(manifest_path).read_text(), "manifest")
    data = Path(events_path).read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    for name, entry in manifest.get("outputs", {}).items():
        if name.endswith("/events.jsonl") and entry.get("sha256") == digest and entry.get("bytes") == len(data):
            return name
    raise Malformed("original events file is not a pinned capture in the startup-original manifest")

def original_timeline(path):
    events, draws = [], 0
    for n, line in enumerate(Path(path).read_text().splitlines(), 1):
        d = strict_loads(line, f"{path}:{n}")
        ev, fn = d.get("ev"), d.get("fn")
        if ev == "rand":
            draws += 1
        elif ev == "exit" and fn == "0x1cf008":
            r = d.get("result")
            if not isinstance(r, dict) or type(r.get("id")) is not int:
                raise Malformed(f"{path}:{n}: allocation exit without a result id")
            events.append(["alloc", r["id"], draws])
        elif ev == "enter" and fn == "0x1c9bb8":
            r = d.get("desc")
            if not isinstance(r, dict) or type(r.get("id")) is not int:
                raise Malformed(f"{path}:{n}: upload without a destination descriptor")
            events.append(["upload", r["id"], draws])
    return events, draws

def native_timeline(directory):
    state = strict_loads((Path(directory) / "state.json").read_text(), "native state.json")
    if state.get("coverage") != "pre-scheduler-only" or type(state.get("rng_draws")) is not int:
        raise Malformed("native report lacks pre-scheduler coverage or rng_draws")
    events, phases = [], []
    for n, line in enumerate((Path(directory) / "timeline.jsonl").read_text().splitlines(), 1):
        d = strict_loads(line, f"timeline.jsonl:{n}")
        kind = d.get("kind")
        if kind in ("alloc", "upload"):
            if set(d) != {"kind", "alloc", "tbp", "draws", "rng_state"} or not all(type(d[k]) is int for k in ("alloc", "tbp", "draws", "rng_state")):
                raise Malformed(f"timeline.jsonl:{n}: bad {kind} entry")
            events.append([kind, d["alloc"], d["draws"]])
        elif kind in ("phase-begin", "phase-end"):
            if set(d) != {"kind", "phase", "draws", "rng_state"} or not isinstance(d["phase"], str):
                raise Malformed(f"timeline.jsonl:{n}: bad phase entry")
            phases.append([kind, d["phase"], d["draws"]])
        else:
            raise Malformed(f"timeline.jsonl:{n}: unknown kind {kind!r}")
    return events, state["rng_draws"], phases

def run(original_events, native_dir, out=None, manifest=MANIFEST):
    out = out or sys.stdout
    capture = pinned(original_events, manifest)
    orig, orig_draws = original_timeline(original_events)
    nat, nat_draws, phases = native_timeline(native_dir)
    for side, ev, dr in (("original", orig, orig_draws), ("native", nat, nat_draws)):
        counts = {"alloc": sum(e[0] == "alloc" for e in ev), "upload": sum(e[0] == "upload" for e in ev), "draws": dr}
        if counts != EXPECTED:
            raise Malformed(f"{side} timeline totals {counts} differ from the contract {EXPECTED}")
    print(f"original capture: {capture}", file=out)
    first = next((i for i in range(len(orig)) if orig[i] != nat[i]), None)
    for name, begin in (("noise", None), ("effect-preparation", None), ("fonts", None)):
        mark = [p for p in phases if p[1] == name]
        print(f"  native phase {name}: " + ", ".join(f"{k} at {d} draws" for k, _, d in mark), file=out)
    print(f"  events: {len(orig)} each; draws: original {orig_draws}, native {nat_draws}", file=out)
    if first is not None:
        print(f"  first difference at event {first}: original {orig[first]} | native {nat[first]}", file=out)
    identical = first is None and orig_draws == nat_draws
    print("result: " + ("IDENTICAL" if identical else "DIFFERENT"), file=out)
    return 0 if identical else 1

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
