#!/usr/bin/env python3
"""Compare two Handle lifecycle traces (the native handle lifecycle trace schema).

usage: texture_handle_trace_diff.py ORIGINAL.jsonl NATIVE.jsonl [--ignore FIELD]...

Both files are JSON-lines: a leading init line, then one object per op. Lines
are compared in order; objects are flattened one level (``vram`` becomes
``vram.available``, ``vram.cursors`` ...), every other field is compared as a
whole and, for lists, the first differing element is reported.

--ignore FIELD (repeatable, never defaulted) drops a field (or a whole group:
``vram`` drops all ``vram.*``). Ignoring a name that matches nothing in either
trace is reported on stderr, because a typo would silently hide differences.

Exit status: 0 identical, 1 at least one difference (including different line
counts), 2 malformed input or usage error.

Two modes:
  generic (default)  structural line-by-line comparison of any two JSON-lines
                     files; for exploration, never for acceptance.
  --accept SCRIPT    acceptance gate. Both traces must satisfy the documented
                     schema EXACTLY (exact key sets, exact types, no bool-as-int,
                     no floats/NaN/Infinity, no duplicate JSON keys, init line
                     op -1, contiguous op numbers 0..n-1, `text` equal to the
                     script's n-th operation, line count = operations + 1) before
                     any comparison, so empty, truncated, reordered or
                     schema-less captures cannot pass. --ignore is refused and
                     no field is flattened (dotted names cannot collide).
                     Exit 2 on any schema violation.
"""
import argparse
import json
import re
import sys
from pathlib import Path

class MalformedTrace(Exception):
    pass

def canon(value):
    """Type-strict canonical form: 0, false and 0.0 must not compare equal."""
    return json.dumps(value, sort_keys=True, separators=(',', ':'))

def _no_duplicates(pairs):
    out = {}
    for key, value in pairs:
        if key in out:
            raise ValueError(f"duplicate key {key!r}")
        out[key] = value
    return out

def _reject_constant(name):
    raise ValueError(f"non-finite number {name}")

def _reject_float(text):
    raise ValueError(f"floating-point number {text} (traces use integers only)")

def load_trace(path, strict=False):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            raw = handle.read()
    except (OSError, UnicodeDecodeError) as exc:
        raise MalformedTrace(f"{path}: cannot read: {exc}")
    lines = raw.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    records = []
    for number, text in enumerate(lines, 1):
        try:
            if strict:
                value = json.loads(text, object_pairs_hook=_no_duplicates,
                                   parse_constant=_reject_constant, parse_float=_reject_float)
            else:
                value = json.loads(text, object_pairs_hook=_no_duplicates)
        except ValueError as exc:
            raise MalformedTrace(f"{path}:{number}: invalid JSON: {exc}")
        if not isinstance(value, dict):
            raise MalformedTrace(f"{path}:{number}: line is not a JSON object")
        records.append(value)
    return records

HEX64 = re.compile(r"[0-9a-f]{64}")
SCRIPT_TOP_KEYS = {"op", "text", "result", "handles", "records", "hash", "live_handles", "reserved",
                   "free_records", "free_extras", "descriptors", "vram", "file_requests"}
RECORD_KEYS = {"r", "image", "pal0", "name", "hash_slot", "handles", "extra"}
VRAM_KEYS = {"available", "cursors", "occupancy_sha256", "touched", "touched_sha256"}

def is_int(value):
    return type(value) is int

def script_operations(path):
    """Operation texts exactly as both runners emit them (trimmed, # and blank skipped)."""
    try:
        with open(path, "r", encoding="utf-8") as handle:
            raw = handle.read()
    except (OSError, UnicodeDecodeError) as exc:
        raise MalformedTrace(f"{path}: cannot read script: {exc}")
    ops = []
    for line in raw.split("\n"):
        text = line.strip(" \t\r")
        if text and not text.startswith("#"):
            ops.append(text)
    if not ops:
        raise MalformedTrace(f"{path}: script has no operations")
    return ops

def validate_record(record, expected_op, expected_text, where):
    """Raise MalformedTrace unless `record` satisfies the trace schema exactly."""
    def bad(msg):
        raise MalformedTrace(f"{where}: {msg}")

    def pairs(name, value, ascending_first=False):
        if not isinstance(value, list):
            bad(f"{name} is not a list")
        last = None
        for item in value:
            if not (isinstance(item, list) and len(item) == 2 and all(is_int(x) for x in item)):
                bad(f"{name} entry {item!r} is not an [int,int] pair")
            if ascending_first and last is not None and item[0] <= last:
                bad(f"{name} is not strictly ascending by first element")
            last = item[0]

    if not isinstance(record, dict):
        bad("not an object")
    keys = set(record)
    if keys != SCRIPT_TOP_KEYS:
        bad(f"key set mismatch: missing {sorted(SCRIPT_TOP_KEYS - keys)}, unexpected {sorted(keys - SCRIPT_TOP_KEYS)}")
    if not is_int(record["op"]) or record["op"] != expected_op:
        bad(f"op is {record['op']!r}, expected {expected_op}")
    if not isinstance(record["text"], str) or record["text"] != expected_text:
        bad(f"text is {record['text']!r}, expected script operation {expected_text!r}")
    if record["result"] is not None and not is_int(record["result"]):
        bad("result is neither integer nor null")
    for name in ("live_handles", "free_records", "free_extras"):
        if not is_int(record[name]) or record[name] < 0:
            bad(f"{name} is not a non-negative integer")
    pairs("handles", record["handles"], True)
    pairs("hash", record["hash"], True)
    pairs("descriptors", record["descriptors"], True)
    if not isinstance(record["reserved"], list) or not all(is_int(x) for x in record["reserved"]):
        bad("reserved is not a list of integers")
    if not isinstance(record["file_requests"], list) or not all(isinstance(x, str) for x in record["file_requests"]):
        bad("file_requests is not a list of strings")
    recs = record["records"]
    if not isinstance(recs, list):
        bad("records is not a list")
    last = None
    for item in recs:
        if not isinstance(item, dict) or set(item) != RECORD_KEYS:
            bad(f"record entry has wrong key set: {item!r}"[:200])
        for name in ("r", "image", "pal0", "hash_slot", "handles"):
            if not is_int(item[name]):
                bad(f"record field {name} is not an integer")
        if not isinstance(item["name"], str):
            bad("record name is not a string")
        if not isinstance(item["extra"], list) or not all(is_int(x) for x in item["extra"]):
            bad("record extra is not a list of integers")
        if last is not None and item["r"] <= last:
            bad("records are not strictly ascending by r")
        last = item["r"]
    if record["live_handles"] != len(record["handles"]):
        bad("live_handles does not equal the number of mapped handles")
    if record["free_records"] + len(recs) != 512:
        bad("free_records + live records != 512")
    vram = record["vram"]
    if not isinstance(vram, dict) or set(vram) != VRAM_KEYS:
        bad("vram key set mismatch")
    if not is_int(vram["available"]) or not is_int(vram["touched"]):
        bad("vram available/touched are not integers")
    if not (isinstance(vram["cursors"], list) and len(vram["cursors"]) == 64 and all(is_int(x) for x in vram["cursors"])):
        bad("vram cursors are not 64 integers")
    for name in ("occupancy_sha256", "touched_sha256"):
        if not isinstance(vram[name], str) or not HEX64.fullmatch(vram[name]):
            bad(f"vram {name} is not 64 lowercase hex digits")

def validate_trace(records, ops, path):
    if len(records) != len(ops) + 1:
        raise MalformedTrace(f"{path}: {len(records)} line(s), expected {len(ops) + 1} "
                             f"(init + {len(ops)} script operation(s)); empty or truncated capture")
    validate_record(records[0], -1, "init", f"{path}:1")
    for index, text in enumerate(ops):
        validate_record(records[index + 1], index, text, f"{path}:{index + 2}")

def accept(original_path, native_path, script_path, out=None):
    """Acceptance comparison. Returns 0 identical, 1 different; raises MalformedTrace."""
    out = out or sys.stdout
    ops = script_operations(script_path)
    if Path(original_path).resolve() == Path(native_path).resolve():
        raise MalformedTrace("original and native captures are the same file: acceptance needs two captures")
    original = load_trace(original_path, strict=True)
    native = load_trace(native_path, strict=True)
    validate_trace(original, ops, original_path)
    validate_trace(native, ops, native_path)
    differing = [i for i in range(len(original)) if canon(original[i]) != canon(native[i])]
    for index in differing[:20]:
        a, b = original[index], native[index]
        for key in sorted(a):
            if key == "vram":
                for sub in sorted(a[key]):
                    if canon(a[key][sub]) != canon(b[key][sub]):
                        print(f"line {index + 1} ({a['text']}): vram.{sub}: original {short(a[key][sub])} | native {short(b[key][sub])}", file=out)
            elif canon(a[key]) != canon(b[key]):
                print(f"line {index + 1} ({a['text']}): {key}: original {short(a[key])} | native {short(b[key])}", file=out)
    print("--- acceptance summary ---", file=out)
    print(f"script operations: {len(ops)}; validated lines per trace: {len(original)}", file=out)
    print(f"differing lines: {len(differing)}", file=out)
    print("result: " + ("IDENTICAL" if not differing else "DIFFERENT"), file=out)
    return 0 if not differing else 1

def flatten(record):
    out = {}
    for key, value in record.items():
        if key == "vram" and isinstance(value, dict):
            for sub, subvalue in value.items():
                out[f"vram.{sub}"] = subvalue
        else:
            out[key] = value
    return out

def ignored(field, ignores):
    return any(field == ig or field.startswith(ig + ".") for ig in ignores)

def short(value, limit=160):
    text = json.dumps(value, separators=(",", ":"), sort_keys=True)
    return text if len(text) <= limit else text[: limit - 3] + "..."

def list_detail(a, b):
    if not (isinstance(a, list) and isinstance(b, list)):
        return None
    if len(a) != len(b):
        head = f"length {len(a)} vs {len(b)}"
    else:
        head = ""
    for index in range(min(len(a), len(b))):
        if canon(a[index]) != canon(b[index]):
            return (head + "; " if head else "") + f"first differing element [{index}]: {short(a[index])} vs {short(b[index])}"
    if len(a) != len(b):
        longer = a if len(a) > len(b) else b
        side = "original" if len(a) > len(b) else "native"
        return head + f"; extra element [{min(len(a), len(b))}] only in {side}: {short(longer[min(len(a), len(b))])}"
    return None

def compare_records(a, b, ignores):
    """Return [(field, original, native, detail)] for one line pair."""
    fa, fb = flatten(a), flatten(b)
    diffs = []
    for field in sorted(set(fa) | set(fb)):
        if ignored(field, ignores):
            continue
        in_a, in_b = field in fa, field in fb
        if in_a and in_b:
            if canon(fa[field]) != canon(fb[field]):
                diffs.append((field, fa[field], fb[field], list_detail(fa[field], fb[field])))
        else:
            diffs.append((field, fa.get(field, "<missing>"), fb.get(field, "<missing>"), "field present in only one trace"))
    return diffs

def diff_traces(original, native, ignores):
    """Return (per_line_diffs, length_note). per_line_diffs: [(index, text, diffs)]."""
    per_line = []
    for index in range(min(len(original), len(native))):
        diffs = compare_records(original[index], native[index], ignores)
        if diffs:
            per_line.append((index, native[index].get("text", original[index].get("text", "?")), diffs))
    length_note = None
    if len(original) != len(native):
        length_note = (len(original), len(native))
    return per_line, length_note

def known_fields(*traces):
    names = set()
    for trace in traces:
        for record in trace:
            names.update(flatten(record))
    return names

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("original")
    parser.add_argument("native")
    parser.add_argument("--ignore", action="append", default=[], metavar="FIELD",
                        help="field (or group such as vram) to leave out; repeatable; no defaults")
    parser.add_argument("--accept", metavar="SCRIPT",
                        help="acceptance mode: strict schema and script-length validation of both traces")
    parser.add_argument("--max-lines", type=int, default=20, help="differing ops to print in detail (default 20)")
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return 2 if exc.code not in (0, None) else 0
    if args.accept is not None:
        if args.ignore:
            print("malformed: --ignore is not permitted in acceptance mode", file=sys.stderr)
            return 2
        try:
            return accept(args.original, args.native, args.accept)
        except MalformedTrace as exc:
            print(f"malformed: {exc}", file=sys.stderr)
            return 2
    try:
        original = load_trace(args.original)
        native = load_trace(args.native)
    except MalformedTrace as exc:
        print(f"malformed: {exc}", file=sys.stderr)
        return 2

    names = known_fields(original, native)
    for ig in args.ignore:
        if not any(n == ig or n.startswith(ig + ".") for n in names):
            print(f"warning: --ignore {ig} matches no field in either trace", file=sys.stderr)

    per_line, length_note = diff_traces(original, native, args.ignore)

    first_by_field = {}
    count_by_field = {}
    for index, text, diffs in per_line:
        for field, a, b, detail in diffs:
            first_by_field.setdefault(field, (index, text, a, b, detail))
            count_by_field[field] = count_by_field.get(field, 0) + 1

    for position, (index, text, diffs) in enumerate(per_line):
        if position >= args.max_lines:
            print(f"... {len(per_line) - position} more differing line(s) not shown")
            break
        label = "init" if index == 0 else f"op {original[index].get('op', index - 1)}"
        print(f"line {index + 1} ({label}: {text}) differs in {len(diffs)} field(s)")
        for field, a, b, detail in diffs:
            print(f"  {field}: original {short(a)} | native {short(b)}")
            if detail:
                print(f"    {detail}")
    if length_note:
        longer = "original" if length_note[0] > length_note[1] else "native"
        print(f"line count differs: original {length_note[0]}, native {length_note[1]} "
              f"({longer} has {abs(length_note[0] - length_note[1])} extra line(s) after the common prefix)")

    compared = min(len(original), len(native))
    print("--- summary ---")
    print(f"lines: original {len(original)}, native {len(native)}, compared {compared}")
    print(f"differing lines: {len(per_line)}")
    if args.ignore:
        print("ignored: " + ", ".join(args.ignore))
    if per_line:
        first_index = per_line[0][0]
        print(f"first difference: line {first_index + 1} ({per_line[0][1]})")
    for field in sorted(first_by_field, key=lambda f: (first_by_field[f][0], f)):
        index, text, _, _, _ = first_by_field[field]
        print(f"  field {field}: first at line {index + 1} ({text}), {count_by_field[field]} line(s)")
    identical = not per_line and not length_note
    print("result: " + ("IDENTICAL" if identical else "DIFFERENT"))
    return 0 if identical else 1

if __name__ == "__main__":
    sys.exit(main())
