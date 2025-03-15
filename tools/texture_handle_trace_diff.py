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
"""
import argparse
import json
import sys

class MalformedTrace(Exception):
    pass

def canon(value):
    """Type-strict canonical form: 0, false and 0.0 must not compare equal."""
    return json.dumps(value, sort_keys=True, separators=(',', ':'))

def load_trace(path):
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
            value = json.loads(text)
        except ValueError as exc:
            raise MalformedTrace(f"{path}:{number}: invalid JSON: {exc}")
        if not isinstance(value, dict):
            raise MalformedTrace(f"{path}:{number}: line is not a JSON object")
        records.append(value)
    return records

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
    parser.add_argument("--max-lines", type=int, default=20, help="differing ops to print in detail (default 20)")
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return 2 if exc.code not in (0, None) else 0
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
