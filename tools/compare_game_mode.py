#!/usr/bin/env python3
"""Compare bounded native scheduler rows with original parent-entry snapshots."""

from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys
from collections import Counter

SCHEMA = "okage-game-mode-observer-v1"
PARENT_ENTRY_PC = "0x00124818"
RECORD_WORDS = 25
RECORD_BYTES = RECORD_WORDS * 4
WORD_NAMES = (
    "phase", "current_mode", "arg1", "arg2", "request_mode", "queued_arg1",
    "queued_arg2", "has_child", "dead_linked", "pending_740", "pending_744",
    "pending_748",
)
NATIVE_COMPARE_INDEXES = (2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14)
REFERENCE_FIELDS = (
    "parent_phase20", "parent_current40", "parent_arg44", "parent_arg4c",
    "parent_request3c", "parent_queued48", "parent_queued50",
    "parent_child38", "parent_linked_first18",
    "pending_24f740", "pending_24f744", "pending_24f748",
)
UNSUPPORTED_NATIVE_FIELDS = [
    {"index": 11, "name": "field_present", "reason": "No matching original parent-entry field is in this comparison."},
    {"index": 15, "name": "fade_current", "reason": "No original fade observation is compared here."},
    {"index": 16, "name": "fade_target", "reason": "No original fade observation is compared here."},
    {"index": 17, "name": "fade_step", "reason": "No original fade observation is compared here."},
    {"index": 18, "name": "capture_alpha", "reason": "No original capture observation is compared here."},
    {"index": 19, "name": "capture_target", "reason": "No original capture observation is compared here."},
    {"index": 20, "name": "frozen_frame", "reason": "No original capture observation is compared here."},
    {"index": 21, "name": "history_frame", "reason": "No original capture observation is compared here."},
    {"index": 22, "name": "clear_rgb_r", "reason": "No original capture observation is compared here."},
    {"index": 23, "name": "clear_rgb_g", "reason": "No original capture observation is compared here."},
    {"index": 24, "name": "clear_rgb_b", "reason": "No original capture observation is compared here."},
]

class ComparisonError(ValueError):
    pass

def _hex_word(value: object, label: str) -> int:
    if not isinstance(value, str) or not value.startswith("0x"):
        raise ComparisonError(f"{label} must be a hexadecimal string")
    try:
        parsed = int(value, 16)
    except ValueError as exc:
        raise ComparisonError(f"{label} is not valid hexadecimal") from exc
    if not 0 <= parsed <= 0xFFFFFFFF:
        raise ComparisonError(f"{label} is outside the u32 range")
    return parsed

def load_reference(path: pathlib.Path) -> dict[int, tuple[int, ...]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise ComparisonError(f"Cannot read reference JSONL: {exc}") from exc
    if not lines:
        raise ComparisonError("Reference JSONL is empty")
    try:
        header = json.loads(lines[0])
    except json.JSONDecodeError as exc:
        raise ComparisonError(f"Reference JSONL header is invalid: {exc}") from exc
    if not isinstance(header, dict) or header.get("schema") != SCHEMA:
        raise ComparisonError(f"Reference JSONL schema must be {SCHEMA}")

    parents: dict[int, tuple[int, ...]] = {}
    for line_number, line in enumerate(lines[1:], start=2):
        if not line.strip():
            raise ComparisonError(f"Reference JSONL has a blank record at line {line_number}")
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ComparisonError(f"Reference JSONL line {line_number} is invalid: {exc}") from exc
        if not isinstance(row, dict):
            raise ComparisonError(f"Reference JSONL line {line_number} is not an object")
        if row.get("pc") != PARENT_ENTRY_PC:
            continue
        frame = row.get("frame")
        if not isinstance(frame, int) or frame < 0:
            raise ComparisonError(f"Reference parent-entry line {line_number} has invalid frame")
        if frame in parents:
            raise ComparisonError(f"Duplicate original parent-entry frame {frame}")
        values = [_hex_word(row.get(name), f"line {line_number} {name}") for name in REFERENCE_FIELDS]
        phase, current, arg1, arg2, request, queued1, queued2, child, linked, *pending = values
        parents[frame] = (
            phase, current, arg1, arg2, request, queued1, queued2,
            int(child != 0), int(linked != 0 and child == 0), *pending,
        )
    if not parents:
        raise ComparisonError("Reference has no parent-entry observations at PC 0x124818")
    return parents

def load_native(path: pathlib.Path, anchor: int) -> list[tuple[int, ...]]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ComparisonError(f"Cannot read native binary: {exc}") from exc
    if not data:
        raise ComparisonError("Native game-mode binary is empty")
    if len(data) % RECORD_BYTES:
        raise ComparisonError(
            f"Native game-mode binary has {len(data)} bytes; expected exact {RECORD_BYTES}-byte records"
        )
    rows = [tuple(words) for words in struct.iter_unpack("<25I", data)]
    previous: tuple[int, int] | None = None
    seen: set[tuple[int, int]] = set()
    for index, row in enumerate(rows):
        frame, point = row[0], row[1]
        if point not in (0, 1):
            raise ComparisonError(f"Native record {index} has invalid point {point}; expected 0 or 1")
        if frame < anchor:
            raise ComparisonError(f"Native record {index} frame {frame} precedes anchor {anchor}")
        key = (frame, point)
        if key in seen:
            raise ComparisonError(f"Duplicate native frame/point record {key}")
        if previous is not None and key <= previous:
            raise ComparisonError(f"Native records are not strictly monotonic at record {index}: {key}")
        seen.add(key)
        previous = key
    return rows

def compare(reference: dict[int, tuple[int, ...]], native: list[tuple[int, ...]], anchor: int) -> dict:
    checks = []
    missing = []
    mismatch_counts: Counter[str] = Counter()
    first_difference = None
    for row_index, row in enumerate(native):
        native_frame, point = row[0], row[1]
        rawframe = native_frame - anchor
        original_raw_frame = rawframe + (1 if point == 1 else 0)
        original_absolute_frame = anchor + original_raw_frame
        expected = reference.get(original_raw_frame)
        check = {
            "native_record": row_index,
            "native_frame": native_frame,
            "point": point,
            "mapping": {
                "anchor": anchor,
                "rawframe": rawframe,
                "original_parent_entry_frame": original_absolute_frame,
                "original_jsonl_frame": original_raw_frame,
                "rule": "point 0 compares the same frame; point 1 compares the next original parent entry",
            },
        }
        actual = tuple(row[index] for index in NATIVE_COMPARE_INDEXES)
        check["native"] = {name: value for name, value in zip(WORD_NAMES, actual)}
        if expected is None:
            check["missing_original"] = True
            missing.append({"native_record": row_index, "original_parent_entry_frame": original_absolute_frame,
                            "original_jsonl_frame": original_raw_frame})
            checks.append(check)
            continue
        diffs = []
        for name, native_value, original_value in zip(WORD_NAMES, actual, expected):
            if native_value != original_value:
                mismatch_counts[name] += 1
                diffs.append({"field": name, "native": f"0x{native_value:08x}",
                              "original": f"0x{original_value:08x}"})
        check["original"] = {name: value for name, value in zip(WORD_NAMES, expected)}
        check["differences"] = diffs
        check["equal"] = not diffs
        if diffs and first_difference is None:
            first_difference = {
                "native_record": row_index,
                "native_frame": native_frame,
                "point": point,
                "original_parent_entry_frame": original_absolute_frame,
                "difference": diffs[0],
            }
        checks.append(check)
    compared = sum("original" in row for row in checks)
    total_differences = sum(mismatch_counts.values())
    return {
        "schema": "okage-game-mode-comparison-v1",
        "anchor": anchor,
        "scope": "Native parent scheduler state compared to original parent-entry observations at 0x124818.",
        "alignment": "Native frame minus anchor gives rawframe. Point 0 maps to anchor+rawframe; point 1 maps to anchor+rawframe+1.",
        "native_records": len(native),
        "original_parent_entry_records": len(reference),
        "compared_records": compared,
        "missing_original_records": len(missing),
        "missing": missing,
        "difference_words": total_differences,
        "difference_counts_by_field": dict(sorted(mismatch_counts.items())),
        "first_difference": first_difference,
        "unsupported_checks": UNSUPPORTED_NATIVE_FIELDS,
        "checks": checks,
        "equal": not missing and total_differences == 0,
    }

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True, type=pathlib.Path, help="Observer JSONL from the original run")
    parser.add_argument("--native", required=True, type=pathlib.Path, help="Raw little-endian 25-u32 .game-mode.bin")
    parser.add_argument("--anchor", required=True, type=int, help="Absolute anchor frame, e.g. 104376")
    parser.add_argument("--output", required=True, type=pathlib.Path, help="New comparison JSON output")
    args = parser.parse_args(argv)
    if args.anchor < 0 or args.anchor > 0xFFFFFFFF:
        parser.error("--anchor must be an unsigned 32-bit frame")
    if args.output.exists() or args.output.is_symlink():
        parser.error(f"Refusing existing output: {args.output}")
    try:
        reference = load_reference(args.reference)
        native = load_native(args.native, args.anchor)
        report = compare(reference, native, args.anchor)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as out:
            json.dump(report, out, indent=2, sort_keys=True)
            out.write("\n")
    except (ComparisonError, OSError) as exc:
        print(f"compare_game_mode: {exc}", file=sys.stderr)
        return 2
    print(f"records={report['native_records']} compared={report['compared_records']} "
          f"missing_original={report['missing_original_records']} differences={report['difference_words']} "
          f"equal={str(report['equal']).lower()} output={args.output}")
    return 0 if report["equal"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
