#!/usr/bin/env python3
"""Compare the native startup texture report with the unchanged-retail startup capture.

usage: startup_compare.py ORIGINAL_ANALYSIS_DIR NATIVE_REPORT_DIR

The original files are verified against the sha256/size pins in docs/startup-original-manifest.json
first, and both sides must hold exactly 79 allocations, 36 handles and (original mask) 444160 written
nibbles, so empty or hand-made inputs cannot pass.

ORIGINAL_ANALYSIS_DIR holds final-state.jsonl, vram-written-mask.bin and vram-written-values.bin
(the startup original-reference contract). NATIVE_REPORT_DIR is the output of
`okage-native --startup-texture-report` (state.json, vram.bytes, vram.known).

Compared (all must agree): allocation table in allocation order (class, psm, width, height, TBP, TBW,
references, packed descriptor), descriptor reference list, handle->record map, every record (image,
palette 0, extra chain, name, hash slot, handle count), name hash, reserved queue, live handle count,
free record/extra counts, available count, the 64 allocator cursors, the occupancy bitmap SHA-256, and
the whole 4 MiB GS local memory: written-nibble mask and the value of every written nibble.

Explicit non-comparisons are printed (rand state after the prefix, free-queue node order).
Exit status: 0 identical, 1 differences, 2 malformed input. Strict: unknown/missing keys, wrong types and
short VRAM images are malformed, never a pass.
"""
import hashlib
import json
import sys
from pathlib import Path

class Malformed(Exception):
    pass

def load_json(path, line_only=False):
    try:
        text = Path(path).read_text()
        if line_only:
            lines = [l for l in text.split("\n") if l]
            if len(lines) != 1:
                raise Malformed(f"{path}: expected exactly one JSON line")
            text = lines[0]

        def no_dup(pairs):
            out = {}
            for k, v in pairs:
                if k in out:
                    raise ValueError(f"duplicate key {k}")
                out[k] = v
            return out

        return json.loads(text, object_pairs_hook=no_dup,
                          parse_constant=lambda c: (_ for _ in ()).throw(ValueError("non-finite " + c)))
    except (OSError, ValueError) as exc:
        raise Malformed(f"{path}: {exc}")

def need(d, keys, where, exact=False):
    if not isinstance(d, dict) or set(keys) - set(d):
        raise Malformed(f"{where}: missing keys {sorted(set(keys) - set(d if isinstance(d, dict) else []))}")
    if exact and set(d) - set(keys):
        raise Malformed(f"{where}: unknown keys {sorted(set(d) - set(keys))}")
    return d

def is_int(v):
    return type(v) is int

def check_ints(v, where, n=None):
    return ints(v, where, n)

def ints(v, where, n=None):
    if not isinstance(v, list) or any(type(x) is not int for x in v) or (n is not None and len(v) != n):
        raise Malformed(f"{where}: not a list of {'%d ' % n if n else ''}integers")
    return v

def desc_id(d, where):
    if d is None:
        return -1
    ints(d, where, 2)
    return d[0]

MANIFEST = Path(__file__).resolve().parents[1] / "docs/startup-original-manifest.json"

def verify_original(orig_dir, manifest_path):
    """The original analysis files must be the pinned captures (sha256 + size from the manifest)."""
    manifest = load_json(manifest_path)
    outputs = manifest.get("outputs")
    digest = manifest.get("result_digest")
    if not isinstance(outputs, dict) or not isinstance(digest, dict):
        raise Malformed("original manifest lacks outputs/result_digest")
    for name in ("final-state.jsonl", "vram-written-mask.bin", "vram-written-values.bin"):
        pinned = outputs.get("analysis/" + name)
        if not pinned:
            raise Malformed(f"manifest does not pin analysis/{name}")
        data = (Path(orig_dir) / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != pinned["sha256"] or len(data) != pinned["bytes"]:
            raise Malformed(f"{name} does not match the pinned capture in the manifest")
    return digest

def normalise_original(o):
    need(o, ["handles", "records", "hash", "live_handles", "reserved", "free_records", "free_extras", "descriptors",
             "allocations", "vram", "rand_state_214ec8_58"], "original")
    allocs = []
    for a in o["allocations"]:
        need(a, ["id", "class", "desc", "refs", "live"], "original allocation")
        d = need(a["desc"], ["raw", "tbp", "tbw", "psm", "w", "h"], "original descriptor")
        allocs.append({"id": a["id"], "class": a["class"], "psm": d["psm"], "width": d["w"], "height": d["h"],
                       "tbp": d["tbp"], "tbw": d["tbw"], "references": a["refs"], "descriptor": int(d["raw"], 16)})
    recs = {r["r"]: {"image": r["image"], "pal0": r["pal0"], "extra": r["extra"], "name": r["name"],
                     "hash_slot": r["hash_slot"], "handles": r["handles"]} for r in o["records"]}
    v = need(o["vram"], ["available", "cursors", "occupancy_sha256"], "original vram")
    return {"allocations": allocs, "handles": [list(h) for h in o["handles"]], "records": recs,
            "hash": [list(h) for h in o["hash"]], "reserved": o["reserved"], "live_handles": o["live_handles"],
            "free_records": o["free_records"], "free_extras": o["free_extras"],
            "descriptors": [list(d) for d in o["descriptors"]], "available": v["available"],
            "cursors": v["cursors"], "occupancy_sha256": v["occupancy_sha256"]}

def normalise_native(n):
    need(n, ["coverage", "later_history", "rand_state_after_noise", "checkpoints", "allocations", "layers",
             "filter_cluts", "handles", "records", "hash", "reserved", "free_records", "free_extras", "available",
             "cursors", "free_queue_ids", "occupancy_hex"], "native", exact=True)
    if not (is_int(n["free_records"]) and is_int(n["free_extras"]) and is_int(n["available"])):
        raise Malformed("native counters are not integers")
    check_ints(n["cursors"], "native cursors", 64)
    check_ints(n["reserved"], "native reserved")
    if not isinstance(n["hash"], list) or any(not (isinstance(p, list) and len(p) == 2 and all(is_int(x) for x in p)) for p in n["hash"]):
        raise Malformed("native hash is not a list of integer pairs")
    if n["coverage"] != "pre-scheduler-only":
        raise Malformed("native report coverage is not pre-scheduler-only: " + str(n["coverage"]))
    allocs = []
    for a in n["allocations"]:
        need(a, ["id", "serial", "class", "psm", "width", "height", "tbp", "tbw", "references", "descriptor"], "native allocation", exact=True)
        if not all(is_int(v) for v in a.values()):
            raise Malformed("native allocation field is not an integer")
        allocs.append({k: a[k] for k in ["id", "class", "psm", "width", "height", "tbp", "tbw", "references", "descriptor"]})
    by_record = {}
    for h in n["handles"]:
        need(h, ["handle", "record", "image", "palettes"], "native handle", exact=True)
        if not (is_int(h["handle"]) and is_int(h["record"]) and isinstance(h["palettes"], list)):
            raise Malformed("native handle field has the wrong type")
        pal = [desc_id(p, "palette") for p in h["palettes"]]
        entry = {"image": desc_id(h["image"], "image"), "pal0": pal[0] if pal else -1, "extra": pal[1:]}
        if by_record.setdefault(h["record"], entry) != entry:
            raise Malformed("native aliases of one record disagree")
    recs = {}
    for r in n["records"]:
        need(r, ["record", "name", "hash_slot", "handles"], "native record", exact=True)
        if not (is_int(r["record"]) and isinstance(r["name"], str) and is_int(r["hash_slot"]) and is_int(r["handles"])):
            raise Malformed("native record field has the wrong type")
        if r["record"] not in by_record:
            raise Malformed("native record without a handle")
        recs[r["record"]] = dict(by_record[r["record"]], name=r["name"], hash_slot=r["hash_slot"], handles=r["handles"])
    try:
        occupancy = hashlib.sha256(bytes.fromhex(n["occupancy_hex"])).hexdigest()
    except ValueError as exc:
        raise Malformed("native occupancy_hex: " + str(exc))
    return {"allocations": allocs, "handles": [[h["handle"], h["record"]] for h in n["handles"]], "records": recs,
            "hash": n["hash"], "reserved": n["reserved"], "live_handles": len(n["handles"]),
            "free_records": n["free_records"], "free_extras": n["free_extras"],
            "descriptors": [[a["id"], a["references"]] for a in allocs], "available": n["available"],
            "cursors": n["cursors"], "occupancy_sha256": occupancy}

def compare(orig, nat, out):
    diffs = 0
    for key in ["allocations", "handles", "hash", "reserved", "live_handles", "free_records", "free_extras",
                "descriptors", "available", "cursors", "occupancy_sha256", "records"]:
        a, b = orig[key], nat[key]
        if json.dumps(a, sort_keys=True) == json.dumps(b, sort_keys=True):
            print(f"  {key}: identical", file=out)
            continue
        diffs += 1
        detail = ""
        if isinstance(a, list) and isinstance(b, list):
            for i in range(max(len(a), len(b))):
                x = a[i] if i < len(a) else "<absent>"
                y = b[i] if i < len(b) else "<absent>"
                if x != y:
                    detail = f" first difference at [{i}]: original {x} | native {y}"
                    break
        elif isinstance(a, dict):
            for k in sorted(set(a) | set(b)):
                if a.get(k) != b.get(k):
                    detail = f" first difference at record {k}: original {a.get(k)} | native {b.get(k)}"
                    break
        else:
            detail = f" original {a} | native {b}"
        print(f"  {key}: DIFFERENT{detail}", file=out)
    return diffs

def compare_vram(orig_dir, nat_dir, out):
    mask = (Path(orig_dir) / "vram-written-mask.bin").read_bytes()
    values = (Path(orig_dir) / "vram-written-values.bin").read_bytes()
    known = (Path(nat_dir) / "vram.known").read_bytes()
    bytes_ = (Path(nat_dir) / "vram.bytes").read_bytes()
    size = 1 << 22
    if not (len(mask) == len(values) == len(known) == len(bytes_) == size):
        raise Malformed("VRAM images must all be 4 MiB")
    bad_mask = bad_value = 0
    first = None
    for i in range(size):
        if mask[i] != known[i]:
            bad_mask += 1
            first = first or (i, mask[i], known[i])
        else:
            m = known[i]
            keep = (0x0f if m & 1 else 0) | (0xf0 if m & 2 else 0)
            if (values[i] & keep) != (bytes_[i] & keep):
                bad_value += 1
                first = first or (i, values[i], bytes_[i])
    written = sum(bin(b).count("1") for b in mask)
    if written != 444160:
        raise Malformed(f"original mask has {written} written nibbles, expected 444160")
    print(f"  vram: {written} written nibbles in the original; mask differences {bad_mask}, value differences {bad_value}"
          + (f"; first at 0x{first[0]:x} ({first[1]:#x} vs {first[2]:#x})" if first else ""), file=out)
    return int(bool(bad_mask or bad_value))

EXPECTED_ALLOCATIONS = 79
EXPECTED_HANDLES = 36

def run(orig_dir, nat_dir, out=None, manifest=MANIFEST):
    out = out or sys.stdout
    digest = verify_original(orig_dir, manifest)
    orig = normalise_original(load_json(Path(orig_dir) / "final-state.jsonl", line_only=True))
    nat = normalise_native(load_json(Path(nat_dir) / "state.json"))
    for side, state in (("original", orig), ("native", nat)):
        if len(state["allocations"]) != EXPECTED_ALLOCATIONS or len(state["handles"]) != EXPECTED_HANDLES:
            raise Malformed(f"{side} does not hold the contract's {EXPECTED_ALLOCATIONS} allocations / {EXPECTED_HANDLES} handles")
    if digest.get("written_nibbles") != 444160:
        raise Malformed("manifest result digest does not carry the expected written-nibble count")
    print("startup texture state, original vs native:", file=out)
    diffs = compare(orig, nat, out) + compare_vram(orig_dir, nat_dir, out)
    print("not compared (no native counterpart): rand state after the prefix (native stops its generator after the noise "
          "texture; 10bc78/10e098 draws are unmodeled), free-queue node order, recorded-but-unkicked display lists", file=out)
    print("result: " + ("IDENTICAL" if not diffs else f"DIFFERENT ({diffs} section(s))"), file=out)
    return 1 if diffs else 0

def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        return run(*argv)
    except (Malformed, KeyError, TypeError, OSError) as exc:
        print(f"malformed: {exc}", file=sys.stderr)
        return 2

if __name__ == "__main__":
    sys.exit(main())
