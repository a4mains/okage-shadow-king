#!/usr/bin/env python3
"""Compare fixed-anchor Restore component observations; not a cold-start parity test."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import sys
from collections import Counter

SOURCE_SCHEMA = "okage-game-mode-observer-v1"
SOURCE_ELF_SHA256 = "2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5"
PARENT_ENTRY_PC = "0x00124818"
RESTORE_CALLBACK_PC = "0x0011c2c8"
RESTORE_WORDS = 978
RESTORE_BYTES = RESTORE_WORDS * 4
GAME_MODE_WORDS = 25
GAME_MODE_BYTES = GAME_MODE_WORDS * 4
NATIVE_OFFSET = 1200

RESTORE_FIELDS = (
    ("signed_state", 5, "global_1f89a8"),
    ("selected", 6, "global_1f89ac"),
    ("completed", 4, "global_24ecc0"),
    ("previous_opacity", 11, "global_24ecc4"),
    ("repeat_delay", 12, "global_24ecd8"),
    ("confirmation", 7, "global_24ee08"),
)

class ComparisonError(ValueError):
    pass

def _u32_hex(value: object, label: str) -> int:
    if not isinstance(value, str) or not value.startswith("0x"):
        raise ComparisonError(f"{label} must be a hexadecimal u32 string")
    try:
        parsed = int(value, 16)
    except ValueError as exc:
        raise ComparisonError(f"{label} is not valid hexadecimal") from exc
    if not 0 <= parsed <= 0xFFFFFFFF:
        raise ComparisonError(f"{label} is outside the u32 range")
    return parsed

def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as exc:
        raise ComparisonError(f"Cannot read {path}: {exc}") from exc
    return digest.hexdigest()

def load_source(path: pathlib.Path, source_stop: int) -> tuple[dict[int, dict], dict[int, int]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise ComparisonError(f"Cannot read source observer JSONL: {exc}") from exc
    if not lines:
        raise ComparisonError("Source observer JSONL is empty")
    try:
        header = json.loads(lines[0])
    except json.JSONDecodeError as exc:
        raise ComparisonError(f"Source observer header is invalid: {exc}") from exc
    if not isinstance(header, dict) or header.get("schema") != SOURCE_SCHEMA:
        raise ComparisonError(f"Source observer schema must be {SOURCE_SCHEMA}")
    if header.get("elf_sha256") != SOURCE_ELF_SHA256:
        raise ComparisonError("Source observer ELF hash is not the pinned USA ELF")

    restore: dict[int, dict] = {}
    parent_phase: dict[int, int] = {}
    for line_number, line in enumerate(lines[1:], start=2):
        if not line.strip():
            raise ComparisonError(f"Source JSONL has a blank row at line {line_number}")
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ComparisonError(f"Source JSONL line {line_number} is invalid: {exc}") from exc
        if not isinstance(row, dict):
            raise ComparisonError(f"Source JSONL line {line_number} is not an object")
        frame = row.get("frame")
        if not isinstance(frame, int) or isinstance(frame, bool) or frame < 0:
            raise ComparisonError(f"Source JSONL line {line_number} has an invalid frame")
        if frame >= source_stop:
            continue
        pc = row.get("pc")
        if pc == PARENT_ENTRY_PC:
            if frame in parent_phase:
                raise ComparisonError(f"Duplicate source parent-entry frame {frame}")
            parent_phase[frame] = _u32_hex(row.get("parent_phase20"), f"line {line_number} parent_phase20")
        observation = row.get("restore")
        if pc == RESTORE_CALLBACK_PC and isinstance(observation, dict) and observation.get("edge") == "second_callback_entry":
            if frame in restore:
                raise ComparisonError(f"Duplicate source Restore second-callback frame {frame}")
            source_values = {
                name: _u32_hex(observation.get(source_key), f"line {line_number} {source_key}")
                for name, _native_index, source_key in RESTORE_FIELDS
            }
            manager_flags = _u32_hex(observation.get("manager_flags1c"), f"line {line_number} manager_flags1c")
            source_values["dead"] = int(bool(manager_flags & 0x80000000))
            restore[frame] = source_values
    if not restore and not parent_phase:
        raise ComparisonError("Source observer has no requested Restore or parent-entry observations")
    return restore, parent_phase

def load_native_restore(path: pathlib.Path) -> dict[int, tuple[int, ...]]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ComparisonError(f"Cannot read native Restore state: {exc}") from exc
    if not data:
        return {}
    if len(data) % RESTORE_BYTES:
        raise ComparisonError(f"Native Restore state has {len(data)} bytes; expected exact {RESTORE_BYTES}-byte records")
    rows: dict[int, tuple[int, ...]] = {}
    previous = -1
    for index, row in enumerate(struct.iter_unpack(f"<{RESTORE_WORDS}I", data)):
        frame = row[0]
        if frame <= previous:
            raise ComparisonError(f"Native Restore frames are not strictly increasing at record {index}")
        if row[2] not in (0, 1) or row[3] not in (0, 1) or row[4] not in (0, 1):
            raise ComparisonError(f"Native Restore record {index} has a non-boolean presence/dead/completed field")
        rows[frame] = tuple(row)
        previous = frame
    return rows

def load_native_game_mode(path: pathlib.Path) -> dict[int, int]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ComparisonError(f"Cannot read native game-mode state: {exc}") from exc
    if not data or len(data) % GAME_MODE_BYTES:
        raise ComparisonError(f"Native game-mode state must contain nonempty exact {GAME_MODE_BYTES}-byte records")
    rows: dict[int, int] = {}
    previous: tuple[int, int] | None = None
    for index, row in enumerate(struct.iter_unpack(f"<{GAME_MODE_WORDS}I", data)):
        frame, point = row[0], row[1]
        key = (frame, point)
        if point not in (0, 1):
            raise ComparisonError(f"Native game-mode record {index} has invalid point {point}")
        if previous is not None and key <= previous:
            raise ComparisonError(f"Native game-mode records are not strictly increasing at record {index}")
        if point == 0:
            rows[frame] = row[2]
        previous = key
    return rows

def compare(source_restore: dict[int, dict], source_parent: dict[int, int],
            native_restore: dict[int, tuple[int, ...]], native_parent: dict[int, int],
            *, native_offset: int, source_stop: int) -> dict:
    if native_offset != NATIVE_OFFSET:
        raise ComparisonError(f"This fixed-anchor comparison requires --native-offset {NATIVE_OFFSET}")
    if not 0 < source_stop <= 0xFFFFFFFF:
        raise ComparisonError("source_stop must be a positive u32 frame boundary")

    restore_checks = []
    restore_mismatches: Counter[str] = Counter()
    restore_missing_source: list[int] = []
    restore_missing_native: list[int] = []
    restore_both_absent: list[int] = []
    both_absent_with_retirement_evidence: list[int] = []
    both_absent_after_last_observation: list[int] = []
    first_restore_difference = None
    compared_restore = 0
    source_active_frames = [frame for frame in source_restore if 0 <= frame < source_stop]
    native_active_source_frames = [frame - native_offset for frame, row in native_restore.items()
                                   if 0 <= frame - native_offset < source_stop and row[2] == 1]
    last_source_active = max(source_active_frames) if source_active_frames else None
    last_native_active = max(native_active_source_frames) if native_active_source_frames else None
    last_source_dead = source_restore[last_source_active]["dead"] if last_source_active is not None else None
    last_native_dead = (native_restore[last_native_active + native_offset][3]
                        if last_native_active is not None else None)
    for source_frame in range(source_stop):
        native_frame = source_frame + native_offset
        source = source_restore.get(source_frame)
        native_row = native_restore.get(native_frame)
        native = native_row if native_row is not None and native_row[2] == 1 else None
        check = {"source_frame": source_frame, "native_frame": native_frame}
        if source is None:
            restore_missing_source.append(source_frame)
        if native is None:
            restore_missing_native.append(source_frame)
        if source is None and native is None:
            restore_both_absent.append(source_frame)
            retirement_observed = (
                last_source_active is not None and last_native_active is not None and
                source_frame > last_source_active and source_frame > last_native_active and
                last_source_dead == 1 and last_native_dead == 1 and native_row is not None and
                native_row[2] == 0 and native_row[3] == 1
            )
            if retirement_observed:
                check["status"] = "both_absent_with_observed_retirement_evidence"
                both_absent_with_retirement_evidence.append(source_frame)
            else:
                check["status"] = "both_absent_after_last_observation"
                both_absent_after_last_observation.append(source_frame)
            restore_checks.append(check)
            continue
        if source is None:
            check["status"] = "source_observation_missing_or_retired"
            restore_checks.append(check)
            continue
        if native is None:
            check["status"] = "native_manager_missing_or_retired"
            restore_checks.append(check)
            continue
        compared_restore += 1
        differences = []
        for name, native_index, _source_key in RESTORE_FIELDS:
            native_value = native[native_index]
            source_value = source[name]
            if native_value != source_value:
                restore_mismatches[name] += 1
                differences.append({"field": name, "source": f"0x{source_value:08x}",
                                    "native": f"0x{native_value:08x}"})
        native_dead = native[3]
        if native_dead != source["dead"]:
            restore_mismatches["dead"] += 1
            differences.append({"field": "dead", "source": source["dead"], "native": native_dead})
        check.update(status="compared", differences=differences)
        if differences and first_restore_difference is None:
            first_restore_difference = {"source_frame": source_frame, "native_frame": native_frame,
                                        "difference": differences[0]}
        restore_checks.append(check)

    parent_checks = []
    parent_mismatches = 0
    parent_missing_source: list[int] = []
    parent_missing_native: list[int] = []
    parent_both_missing: list[int] = []
    first_parent_difference = None
    compared_parent = 0
    for source_frame in range(source_stop):
        native_frame = source_frame + native_offset
        source_value = source_parent.get(source_frame)
        native_value = native_parent.get(native_frame)
        check = {"source_frame": source_frame, "native_frame": native_frame}
        if source_value is None:
            parent_missing_source.append(source_frame)
        if native_value is None:
            parent_missing_native.append(source_frame)
        if source_value is None and native_value is None:
            parent_both_missing.append(source_frame)
            check["status"] = "both_missing"
        elif source_value is None:
            check["status"] = "source_missing"
        elif native_value is None:
            check["status"] = "native_missing"
        else:
            compared_parent += 1
            equal = source_value == native_value
            check.update(status="compared", source_phase=f"0x{source_value:08x}",
                         native_phase=f"0x{native_value:08x}", equal=equal)
            if not equal:
                parent_mismatches += 1
                if first_parent_difference is None:
                    first_parent_difference = {"source_frame": source_frame, "native_frame": native_frame,
                                               "source_phase": f"0x{source_value:08x}",
                                               "native_phase": f"0x{native_value:08x}"}
        parent_checks.append(check)

    manager_value_equal = compared_restore > 0 and not restore_mismatches
    manager_coverage_ok = not restore_missing_source and not restore_missing_native
    parent_value_equal = compared_parent > 0 and parent_mismatches == 0
    parent_coverage_ok = not parent_missing_source and not parent_missing_native
    return {
        "schema": "okage-restore-component-comparison-v1",
        "scope": "Component-only Restore manager state and parent phase comparison; not full cold-start parity or PS2 asynchronous I/O parity.",
        "alignment": {
            "native_offset": native_offset,
            "source_interval": [0, source_stop],
            "rule": "Source frame f maps only to native completed tick f+1200; no anchor fitting or later realignment.",
            "restore_edges": "Source value is second_callback_entry before original 0x11c2c8 (after input handler); native value is completed tick state.",
            "parent_phase_edges": "Source 0x124818 parent entry is before parent update; native game-mode point 0 is sampled in the same update.",
        },
        "restore_state": {
            "compared_records": compared_restore,
            "source_observation_records_in_interval": sum(frame in source_restore for frame in range(source_stop)),
            "native_manager_records_in_interval": sum((frame + native_offset) in native_restore and native_restore[frame + native_offset][2] == 1 for frame in range(source_stop)),
            "difference_words": sum(restore_mismatches.values()),
            "difference_counts_by_field": dict(sorted(restore_mismatches.items())),
            "first_difference": first_restore_difference,
            "missing_source_count": len(restore_missing_source),
            "missing_native_or_retired_count": len(restore_missing_native),
            "both_absent_count": len(restore_both_absent),
            "both_absent_with_observed_retirement_evidence_count": len(both_absent_with_retirement_evidence),
            "both_absent_with_observed_retirement_evidence_source_frames": both_absent_with_retirement_evidence,
            "both_absent_after_last_observation_count": len(both_absent_after_last_observation),
            "both_absent_after_last_observation_source_frames": both_absent_after_last_observation,
            "last_source_manager_observation_frame": last_source_active,
            "last_source_manager_dead_flag": last_source_dead,
            "last_native_manager_observation_source_frame": last_native_active,
            "last_native_manager_dead_flag": last_native_dead,
            "retirement_interpretation": "Absence after the final observation is not itself proof of retirement. Only frames with both last-active dead flags set and a same-frame native manager-present=0/dead=1 record are labeled with observed retirement evidence.",
            "missing_source_frames": restore_missing_source,
            "missing_native_or_retired_source_frames": restore_missing_native,
            "both_absent_or_retired_source_frames": restore_both_absent,
            "values_equal_when_paired": manager_value_equal,
            "coverage_complete": manager_coverage_ok,
            "checks": restore_checks,
            "excluded_native_fields": [
                {"index": 10, "name": "opacity", "reason": "Different source second-callback-entry and native completed-tick edges."},
                {"index": 13, "name": "slot0_alpha", "reason": "Different callback edges."},
                {"index": 16, "name": "slot1_alpha", "reason": "Different callback edges."},
                {"index": 19, "name": "slot2_alpha", "reason": "Different callback edges."},
                {"index": 8, "name": "native_io_enum", "reason": "Host storage service is not PS2 asynchronous I/O state."},
                {"index": 9, "name": "success_ticks", "reason": "Host storage service has no PS2 asynchronous I/O equivalent."},
            ],
        },
        "parent_phase": {
            "compared_records": compared_parent,
            "difference_records": parent_mismatches,
            "first_difference": first_parent_difference,
            "missing_source_count": len(parent_missing_source),
            "missing_native_count": len(parent_missing_native),
            "both_missing_count": len(parent_both_missing),
            "missing_source_frames": parent_missing_source,
            "missing_native_source_frames": parent_missing_native,
            "both_missing_source_frames": parent_both_missing,
            "values_equal_when_paired": parent_value_equal,
            "coverage_complete": parent_coverage_ok,
            "checks": parent_checks,
        },
        "component_values_equal_when_paired": manager_value_equal and parent_value_equal,
        "complete_observation_coverage": manager_coverage_ok and parent_coverage_ok,
    }

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=pathlib.Path, help="Original mode observer JSONL")
    parser.add_argument("--native-restore", required=True, type=pathlib.Path, help="Raw 978-u32 .restore-state.bin")
    parser.add_argument("--native-game-mode", required=True, type=pathlib.Path, help="Raw 25-u32 .game-mode.bin")
    parser.add_argument("--native-offset", required=True, type=int, help="Fixed source-to-native tick offset; must be 1200")
    parser.add_argument("--source-stop", required=True, type=int, help="Exclusive source frame boundary; expected 300")
    parser.add_argument("--output", required=True, type=pathlib.Path, help="New JSON report path")
    args = parser.parse_args(argv)
    if args.output.exists() or args.output.is_symlink():
        parser.error(f"Refusing existing output: {args.output}")
    if args.native_offset != NATIVE_OFFSET:
        parser.error(f"--native-offset must be the fixed value {NATIVE_OFFSET}")
    if args.source_stop != 300:
        parser.error("This retained source capture requires --source-stop 300")
    try:
        source_restore, source_parent = load_source(args.source, args.source_stop)
        native_restore = load_native_restore(args.native_restore)
        native_parent = load_native_game_mode(args.native_game_mode)
        report = compare(source_restore, source_parent, native_restore, native_parent,
                         native_offset=args.native_offset, source_stop=args.source_stop)
        report["inputs"] = {
            "source_jsonl_sha256": sha256_file(args.source),
            "native_restore_sha256": sha256_file(args.native_restore),
            "native_game_mode_sha256": sha256_file(args.native_game_mode),
            "source_elf_sha256": SOURCE_ELF_SHA256,
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except (ComparisonError, OSError) as exc:
        print(f"compare_restore: {exc}", file=sys.stderr)
        return 2
    state = report["restore_state"]
    parent = report["parent_phase"]
    print(f"restore_compared={state['compared_records']} restore_differences={state['difference_words']} "
          f"restore_both_absent={state['both_absent_count']} "
          f"parent_compared={parent['compared_records']} parent_differences={parent['difference_records']} "
          f"output={args.output}")
    return 0 if report["component_values_equal_when_paired"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
