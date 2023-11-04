#!/usr/bin/env python3
"""Compare original results-parent entries with preceding native completed frames."""
from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys
from collections import Counter
from typing import Optional

SCHEMA = "okage-game-mode-observer-v1"
EXTENSION = "source-results-entry-v1"
PARENT_ENTRY_PC = "0x00124818"
WORDS = 16
RECORD_BYTES = WORDS * 4
NATIVE_FIELDS = (
    "phase", "ui_stage", "complete", "counter", "pending", "state", "command",
    "ready", "notice", "row", "wait", "reward_display", "money_display", "currency", "rng",
)
COMPARE_FIELDS = (
    "phase", "ui_stage", "complete", "counter", "state", "command", "ready", "notice",
    "row", "wait", "reward_display", "money_display", "currency", "rng",
)
ORIGINAL_FIELDS = {
    "phase": "field_phase48",
    "ui_stage": "field_ui_stage34",
    "complete": "field_complete30",
    "counter": "field_counter38",
    "state": "state_e4c",
    "command": "command_a8",
    "ready": "loader_ready_2e1974",
    "notice": "notice_1fddd4",
    "row": "notice_row_1fddc0",
    "wait": "notice_wait_1fddc4",
    "reward_display": "reward_display_1fddb4",
    "money_display": "money_display_1fddb8",
    "currency": "currency_bank4_index3",
    "rng": "rng_state",
}

class ComparisonError(ValueError):
    pass

def _u32_hex(value: object, label: str) -> int:
    if not isinstance(value, str) or not value.startswith("0x"):
        raise ComparisonError(f"{label} must be a hexadecimal string")
    try:
        parsed = int(value, 16)
    except ValueError as exc:
        raise ComparisonError(f"{label} is invalid hexadecimal") from exc
    if not 0 <= parsed <= 0xFFFFFFFF:
        raise ComparisonError(f"{label} is outside the u32 range")
    return parsed

def load_reference(path: pathlib.Path, source_anchor: int, source_frames: int) -> dict[int, dict[str, Optional[int]]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise ComparisonError(f"Cannot read original JSONL: {exc}") from exc
    if not lines:
        raise ComparisonError("Original JSONL is empty")
    try:
        header = json.loads(lines[0])
    except json.JSONDecodeError as exc:
        raise ComparisonError(f"Original JSONL header is invalid: {exc}") from exc
    if not isinstance(header, dict) or header.get("schema") != SCHEMA:
        raise ComparisonError(f"Original JSONL schema must be {SCHEMA}")
    if header.get("results_extension") != EXTENSION:
        raise ComparisonError(f"Original JSONL results_extension must be {EXTENSION}")

    parents: dict[int, dict[str, Optional[int]]] = {}
    last_frame = -1
    for line_number, line in enumerate(lines[1:], start=2):
        if not line.strip():
            raise ComparisonError(f"Original JSONL has a blank record at line {line_number}")
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ComparisonError(f"Original JSONL line {line_number} is invalid: {exc}") from exc
        if not isinstance(row, dict):
            raise ComparisonError(f"Original JSONL line {line_number} is not an object")
        if row.get("pc") != PARENT_ENTRY_PC:
            continue
        frame = row.get("frame")
        if isinstance(frame, bool) or not isinstance(frame, int) or frame < 0 or frame >= source_frames:
            raise ComparisonError(f"Original parent-entry line {line_number} has frame outside requested interval")
        if frame in parents:
            raise ComparisonError(f"Duplicate original parent-entry frame {frame}")
        if frame <= last_frame:
            raise ComparisonError(f"Original parent-entry frames are out of order at {frame}")
        last_frame = frame
        results = row.get("results")
        if not isinstance(results, dict) or results.get("edge") != "parent_entry":
            raise ComparisonError(f"Original parent-entry line {line_number} lacks results parent_entry payload")
        manager = results.get("manager_raw_pointer", "__missing__")
        if manager == "__missing__":
            raise ComparisonError(f"Original parent-entry line {line_number} lacks manager_raw_pointer marker")
        if manager is not None:
            _u32_hex(manager, f"line {line_number} manager_raw_pointer")
        source: dict[str, Optional[int]] = {}
        for name in COMPARE_FIELDS:
            key = ORIGINAL_FIELDS[name]
            if name in ("state", "command") and manager is None:
                source[name] = None
                continue
            if key not in results:
                raise ComparisonError(f"Original parent-entry line {line_number} lacks {key}")
            value = results[key]
            if value is None:
                source[name] = None
            else:
                source[name] = _u32_hex(value, f"line {line_number} {key}")
        parents[frame] = source
    if not parents:
        raise ComparisonError(f"Original JSONL has no parent-entry observations at {PARENT_ENTRY_PC}")
    return parents

def load_native(path: pathlib.Path, source_anchor: int, source_frames: int) -> dict[int, tuple[int, ...]]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ComparisonError(f"Cannot read native results binary: {exc}") from exc
    if not data:
        raise ComparisonError("Native results binary is empty")
    if len(data) % RECORD_BYTES:
        raise ComparisonError(f"Native results binary has {len(data)} bytes; expected exact {RECORD_BYTES}-byte records")
    first_frame = source_anchor - 1
    last_frame = first_frame + source_frames - 1
    if first_frame < 0 or last_frame > 0xFFFFFFFF:
        raise ComparisonError("Requested native frame interval is outside u32 range")
    records: dict[int, tuple[int, ...]] = {}
    previous = -1
    for index, row in enumerate(struct.iter_unpack("<16I", data)):
        frame = row[0]
        if frame <= previous:
            raise ComparisonError(f"Native frames are duplicate or out of order at record {index}: {frame}")
        previous = frame
        if frame < first_frame or frame > last_frame:
            raise ComparisonError(f"Native record {index} frame {frame} is outside requested interval [{first_frame},{last_frame}]")
        records[frame] = tuple(row)
    return records

def compare(reference: dict[int, dict[str, Optional[int]]], native: dict[int, tuple[int, ...]],
            source_anchor: int, source_frames: int) -> dict:
    missing_source = []
    missing_native = []
    differences = []
    unknowns = []
    counts: Counter[str] = Counter()
    first_difference = None
    checks = []
    for raw_frame in range(source_frames):
        source_frame = source_anchor + raw_frame
        native_frame = source_frame - 1
        original = reference.get(raw_frame)
        actual = native.get(native_frame)
        item = {
            "raw_frame": raw_frame,
            "source_parent_entry_frame": source_frame,
            "native_preceding_completed_frame": native_frame,
            "mapping": "original parent entry at anchor+raw_frame is compared with native completed frame anchor+raw_frame-1",
        }
        if original is None:
            missing_source.append({"raw_frame": raw_frame, "source_frame": source_frame})
        if actual is None:
            missing_native.append({"raw_frame": raw_frame, "native_frame": native_frame})
        if original is not None and actual is not None:
            word_map = dict(zip(NATIVE_FIELDS, actual[1:]))
            per_frame = []
            for name in COMPARE_FIELDS:
                expected = original[name]
                observed = word_map[name]
                if expected is None:
                    record = {"raw_frame": raw_frame, "source_frame": source_frame,
                              "native_frame": native_frame, "field": name,
                              "reason": "Original observer did not sample this field at this parent entry"}
                    unknowns.append(record)
                    continue
                if expected != observed:
                    counts[name] += 1
                    difference = {"raw_frame": raw_frame, "source_frame": source_frame,
                                  "native_frame": native_frame, "field": name,
                                  "original": f"0x{expected:08x}", "native": f"0x{observed:08x}"}
                    differences.append(difference)
                    per_frame.append(difference)
                    if first_difference is None:
                        first_difference = difference
            item["native"] = {name: value for name, value in word_map.items() if name != "pending"}
            item["original"] = {name: (None if value is None else f"0x{value:08x}")
                                for name, value in original.items()}
            item["differences"] = per_frame
            item["equal"] = not per_frame and not any(x["raw_frame"] == raw_frame for x in unknowns)
        checks.append(item)
    expected_native_frames = source_frames
    complete = not missing_source and not missing_native and not unknowns
    equal = complete and not differences
    return {
        "schema": "okage-results-live-comparison-v1",
        "source_anchor": source_anchor,
        "source_frames": source_frames,
        "native_frame_interval": {"first": source_anchor - 1, "last": source_anchor + source_frames - 2},
        "scope": "Results state fields compared at original global-parent entry and the preceding completed native frame.",
        "boundary_limitations": [
            "The original sample is before parent descendants; native rows are after a completed host frame, so ready/state values may differ across these boundaries.",
            "Native pending-job count has no matching original field and is excluded.",
            "Raw pointers are never compared. Missing source values remain unknown and prevent a complete/equal result.",
            "Only the listed results fields are covered; battle actor/live observers do not establish results-tail UI or state equivalence.",
        ],
        "native_records": len(native),
        "expected_native_records": expected_native_frames,
        "original_parent_entry_records": len(reference),
        "expected_original_parent_entry_records": source_frames,
        "compared_frames": len(checks) - len(missing_source) - len(missing_native),
        "missing_source_records": missing_source,
        "missing_native_records": missing_native,
        "unknown_field_count": len(unknowns),
        "unknown_fields": unknowns,
        "difference_count": len(differences),
        "difference_counts_by_field": dict(sorted(counts.items())),
        "first_difference": first_difference,
        "differences": differences,
        "checks": checks,
        "complete": complete,
        "equal": equal,
    }

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True, type=pathlib.Path, help="Original game-mode observer JSONL")
    parser.add_argument("--native", required=True, type=pathlib.Path, help="Raw little-endian 16-u32 .results.bin")
    parser.add_argument("--source-anchor", required=True, type=int, help="Absolute first original parent-entry frame")
    parser.add_argument("--source-frames", required=True, type=int, help="Exact number of source frames to require")
    parser.add_argument("--output", required=True, type=pathlib.Path, help="Fresh comparison JSON output")
    args = parser.parse_args(argv)
    if args.source_anchor < 1 or args.source_anchor > 0xFFFFFFFF:
        parser.error("--source-anchor must be in [1, 0xffffffff]")
    if args.source_frames <= 0 or args.source_frames > 0xFFFFFFFF:
        parser.error("--source-frames must be positive and fit u32")
    if args.source_anchor + args.source_frames - 1 > 0xFFFFFFFF:
        parser.error("Requested source interval exceeds u32 frames")
    if args.output.exists() or args.output.is_symlink():
        parser.error(f"Refusing existing output: {args.output}")
    try:
        reference = load_reference(args.reference, args.source_anchor, args.source_frames)
        native = load_native(args.native, args.source_anchor, args.source_frames)
        report = compare(reference, native, args.source_anchor, args.source_frames)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as output:
            json.dump(report, output, indent=2, sort_keys=True)
            output.write("\n")
    except (ComparisonError, OSError, struct.error) as exc:
        print(f"compare_results_live: {exc}", file=sys.stderr)
        return 2
    print(f"frames={report['source_frames']} compared={report['compared_frames']} "
          f"missing_source={len(report['missing_source_records'])} "
          f"missing_native={len(report['missing_native_records'])} "
          f"unknown={report['unknown_field_count']} differences={report['difference_count']} "
          f"equal={str(report['equal']).lower()} output={args.output}")
    return 0 if report["equal"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
