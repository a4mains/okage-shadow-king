#!/usr/bin/env python3
"""Compare fixed-anchor field Save observations with native Save-state records.

This compares a bounded set of source-authored Save state at callback entry
against the native session's completed-frame observation. Host filesystem job
state and PS2 storage-device globals are reported as unsupported.
"""

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
INPUT_HANDLER_PC = "0x0011e480"
SECOND_CALLBACK_PC = "0x0011c2c8"
NATIVE_WORDS = 978
NATIVE_BYTES = NATIVE_WORDS * 4

ANCHORS = {
    "choice": {"source_stop": 240, "native_offset": 73655, "input_match": True},
    "open": {"source_stop": 360, "native_offset": 73895, "input_match": False},
    "paired-write": {"source_stop": 840, "native_offset": 73655, "input_match": True},
}

GLOBAL_FIELDS = (
    ("state", "1f89a8", None, 5),
    ("selected", "1f89ac", None, 6),
    ("completed", "24ecc0", None, 4),
    ("previous_opacity", "24ecc4", None, 11),
    ("repeat_delay", "24ecd8", None, 12),
    ("confirmation", "24ee08", None, 7),
    ("alpha_0", "24ee10", 0, 13),
    ("alpha_1", "24ee10", 1, 16),
    ("alpha_2", "24ee10", 2, 19),
)
MANAGER_FIELDS = (
    ("manager_flags1c", 0x1C),
    ("manager_window_count_e40", 0xE40),
    ("manager_layer_index_e48", 0xE48),
    ("manager_dispatch_state_e4c", 0xE4C),
    ("manager_layer_state_cursor_e8c", 0xE8C),
)
SECOND_CALLBACK_BOUNDARY_FIELDS = frozenset({
    # The original trace records 0x11c2c8 on entry, before its writes. Native
    # records are emitted after both dispatcher callbacks have returned.
    "state", "completed", "confirmation", "alpha_0", "alpha_1", "alpha_2",
})

class ComparisonError(ValueError):
    pass

def u32(value: object, label: str) -> int:
    if not isinstance(value, str) or not value.startswith("0x"):
        raise ComparisonError(f"{label} must be a hexadecimal u32 string")
    try:
        result = int(value, 16)
    except ValueError as exc:
        raise ComparisonError(f"{label} is not valid hexadecimal") from exc
    if not 0 <= result <= 0xFFFFFFFF:
        raise ComparisonError(f"{label} is outside the u32 range")
    return result

def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as exc:
        raise ComparisonError(f"Cannot read {path}: {exc}") from exc
    return digest.hexdigest()

def run_summary(path: pathlib.Path) -> dict | None:
    if not path.is_file():
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ComparisonError(f"Cannot read run provenance {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise ComparisonError(f"Run provenance {path} is not an object")
    return {
        "run_json_sha256": sha256_file(path),
        "exit_code": data.get("exit_code"),
        "seconds": data.get("seconds"),
        "binary_sha256": data.get("binary_sha256"),
        "command": data.get("command"),
    }

def value_summary(values: list[str]) -> dict:
    counts = Counter(values)
    numeric = [int(v, 16) for v in values if isinstance(v, str) and v.startswith("0x")]
    return {
        "samples": len(values),
        "distinct_values": len(counts),
        "minimum_u32": f"0x{min(numeric):08x}" if numeric else None,
        "maximum_u32": f"0x{max(numeric):08x}" if numeric else None,
        "first": values[0] if values else None,
        "last": values[-1] if values else None,
        "most_common": [{"value": value, "count": count} for value, count in counts.most_common(5)],
    }

def load_source(path: pathlib.Path, source_stop: int) -> tuple[dict[int, dict], dict[int, int]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise ComparisonError(f"Cannot read source Save JSONL: {exc}") from exc
    if not lines:
        raise ComparisonError("Source Save JSONL is empty")
    try:
        header = json.loads(lines[0])
    except json.JSONDecodeError as exc:
        raise ComparisonError(f"Source Save header is invalid: {exc}") from exc
    if not isinstance(header, dict) or header.get("schema") != SOURCE_SCHEMA:
        raise ComparisonError(f"Source schema must be {SOURCE_SCHEMA}")
    if header.get("elf_sha256") != SOURCE_ELF_SHA256:
        raise ComparisonError("Source ELF hash is not the pinned USA ELF")

    save: dict[int, dict] = {}
    save_order: dict[int, list[str]] = {}
    parent: dict[int, int] = {}
    last_frame = -1
    for line_no, line in enumerate(lines[1:], start=2):
        if not line.strip():
            raise ComparisonError(f"Blank source row at line {line_no}")
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ComparisonError(f"Invalid source JSON line {line_no}: {exc}") from exc
        if not isinstance(row, dict):
            raise ComparisonError(f"Source line {line_no} is not an object")
        frame = row.get("frame")
        if not isinstance(frame, int) or isinstance(frame, bool) or frame < 0:
            raise ComparisonError(f"Source line {line_no} has invalid frame")
        if frame < last_frame:
            raise ComparisonError(f"Source frames are out of order at line {line_no}")
        last_frame = frame
        if frame >= source_stop:
            continue
        pc = row.get("pc")
        if pc == PARENT_ENTRY_PC:
            if frame in parent:
                raise ComparisonError(f"Duplicate source parent frame {frame}")
            mode = u32(row.get("parent_current40"), f"line {line_no} parent_current40")
            if mode != 2:
                raise ComparisonError(f"Source frame {frame} is not field mode 2")
            parent[frame] = u32(row.get("parent_phase20"), f"line {line_no} parent_phase20")
        obs = row.get("save")
        if pc not in (INPUT_HANDLER_PC, SECOND_CALLBACK_PC) or not isinstance(obs, dict):
            continue
        edge = "input_handler_entry" if pc == INPUT_HANDLER_PC else "second_callback_entry"
        if obs.get("owner_kind") != "field_mode2_save" or obs.get("edge") != edge:
            raise ComparisonError(f"Source line {line_no} has inconsistent Save owner/edge")
        if obs.get("parent_mode40") != "0x00000002":
            raise ComparisonError(f"Source line {line_no} Save owner is not field mode 2")
        if u32(obs.get("save_selector_1f89a0"), f"line {line_no} selector") != 0:
            raise ComparisonError(f"Source line {line_no} Save selector is not zero")
        if u32(obs.get("window_callback_9c"), f"line {line_no} window callback") != 0x11C2C8:
            raise ComparisonError(f"Source line {line_no} Save window callback is not 0x0011c2c8")
        if u32(obs.get("window_type_a0"), f"line {line_no} window type") != 8:
            raise ComparisonError(f"Source line {line_no} Save window type is not 8")
        descriptor = obs.get("descriptor2234f0")
        if not isinstance(descriptor, list) or descriptor != [
            "0x0011f668", "0x0011fec8", "0x00120ad0", "0x00120b48"
        ]:
            raise ComparisonError(f"Source line {line_no} Save manager descriptor differs from the pinned table")
        globals_obj = obs.get("source_globals")
        if not isinstance(globals_obj, dict):
            raise ComparisonError(f"Source line {line_no} lacks Save globals")
        values = {}
        for name, addr, array_index, _idx in GLOBAL_FIELDS:
            raw = globals_obj.get(addr)
            if array_index is not None:
                if not isinstance(raw, list) or len(raw) != 3:
                    raise ComparisonError(f"line {line_no} global {addr} must be a three-word array")
                raw = raw[array_index]
            values[name] = u32(raw, f"line {line_no} global {addr}")
        manager = {name: u32(obs.get(name), f"line {line_no} {name}")
                   for name, _off in MANAGER_FIELDS}
        manager["dead"] = int(bool(manager["manager_flags1c"] & 0x80000000))
        storage_context = obs.get("storage_context")
        if storage_context is not None:
            if not isinstance(storage_context, dict):
                raise ComparisonError(f"Source line {line_no} storage_context is malformed")
            storage_context = {
                key: u32(storage_context.get(key), f"line {line_no} storage_context.{key}")
                for key in ("control", "pending", "result")
            }
        storage_globals = {
            key: globals_obj[key] for key in
            ("24ee6c", "24ee70", "24ee74", "24ee78", "24ee7c", "24eea8",
             "24ed80_byte", "24ee98_byte", "24ee99_byte", "24ee9a_half")
            if key in globals_obj
        }
        event = {"values": values, "manager": manager, "storage_context": storage_context,
                 "storage_globals": storage_globals,
                 "input_handler_ordinal": obs.get("input_handler_ordinal"),
                 "second_callback_ordinal": obs.get("second_callback_ordinal")}
        if frame in save:
            if edge in save[frame]:
                raise ComparisonError(f"Duplicate source Save {edge} frame {frame}")
            save[frame][edge] = event
        else:
            save[frame] = {edge: event}
        save_order.setdefault(frame, []).append(edge)
        if save_order[frame] not in (
            ["input_handler_entry"], ["second_callback_entry"],
            ["input_handler_entry", "second_callback_entry"],
        ):
            raise ComparisonError(f"Source Save callbacks are out of order at frame {frame}")
    if not parent:
        raise ComparisonError("Source interval has no parent-entry observations")
    return save, parent

def load_native(path: pathlib.Path) -> dict[int, tuple[int, ...]]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ComparisonError(f"Cannot read native Save state: {exc}") from exc
    if not data or len(data) % NATIVE_BYTES:
        raise ComparisonError(f"Native Save state must contain nonempty exact {NATIVE_BYTES}-byte records")
    rows: dict[int, tuple[int, ...]] = {}
    previous = -1
    for ordinal, values in enumerate(struct.iter_unpack(f"<{NATIVE_WORDS}I", data)):
        row = tuple(values)
        frame = row[0]
        if frame <= previous:
            raise ComparisonError(f"Native frames are not strictly increasing at record {ordinal}")
        if row[2] not in (0, 1) or row[3] not in (0, 1) or row[4] not in (0, 1):
            raise ComparisonError(f"Native record {ordinal} has invalid manager/dead/completed flags")
        rows[frame] = row
        previous = frame
    return rows

def compare(source_save: dict[int, dict], source_parent: dict[int, int], native: dict[int, tuple[int, ...]],
            *, case: str, native_offset: int, source_stop: int) -> dict:
    if case not in ANCHORS:
        raise ComparisonError("case must be choice, open, or paired-write")
    anchor = ANCHORS[case]
    if native_offset != anchor["native_offset"] or source_stop != anchor["source_stop"]:
        raise ComparisonError(f"{case} requires fixed native offset {anchor['native_offset']} and source stop {anchor['source_stop']}")

    events = []
    diffs: Counter[str] = Counter()
    boundary_diffs: Counter[str] = Counter()
    missing_source: list[dict] = []
    missing_native: list[dict] = []
    first_difference = None
    first_comparable_difference = None
    compared = 0
    event_counts = Counter()
    source_active_set = set(source_save)
    for frame in range(source_stop):
        obs = source_save.get(frame)
        if obs is None:
            continue
        for edge in ("input_handler_entry", "second_callback_entry"):
            source_event = obs.get(edge)
            native_frame = frame + native_offset - (edge == "input_handler_entry")
            native_row = native.get(native_frame)
            check = {"source_frame": frame, "edge": edge, "native_frame": native_frame}
            if source_event is None:
                missing_source.append(check.copy())
                check["status"] = "source_save_observation_missing"
                events.append(check)
                continue
            if native_row is None or not native_row[2]:
                missing_native.append(check.copy())
                check["status"] = "native_save_manager_missing"
                events.append(check)
                continue
            compared += 1
            event_counts[edge] += 1
            mismatches = []
            source_values = source_event["values"]
            for name, _addr, _array_index, native_index in GLOBAL_FIELDS:
                native_value = native_row[native_index]
                source_value = source_values[name]
                if native_value != source_value:
                    diffs[name] += 1
                    boundary_sensitive = edge == "second_callback_entry" and name in SECOND_CALLBACK_BOUNDARY_FIELDS
                    if boundary_sensitive:
                        boundary_diffs[name] += 1
                    mismatches.append({"field": name,
                                       "classification": "callback_boundary_sensitive" if boundary_sensitive else "comparable_at_sampled_boundary",
                                       "source": f"0x{source_value:08x}",
                                       "native": f"0x{native_value:08x}"})
            manager_values = source_event["manager"]
            for name, offset in MANAGER_FIELDS:
                if name == "manager_layer_state_cursor_e8c" and edge == "input_handler_entry":
                    continue
                native_index = 22 + offset // 4
                native_value = native_row[native_index]
                source_value = manager_values[name]
                if native_value != source_value:
                    diffs[name] += 1
                    mismatches.append({"field": name, "source": f"0x{source_value:08x}",
                                       "native": f"0x{native_value:08x}"})
            if native_row[3] != manager_values["dead"]:
                diffs["dead"] += 1
                mismatches.append({"field": "dead", "source": manager_values["dead"],
                                   "native": native_row[3]})
            check.update(status="compared", differences=mismatches)
            if mismatches and first_difference is None:
                first_difference = {"source_frame": frame, "edge": edge, "native_frame": native_frame,
                                    "difference": mismatches[0]}
            if first_comparable_difference is None:
                for mismatch in mismatches:
                    if mismatch.get("classification") != "callback_boundary_sensitive":
                        first_comparable_difference = {
                            "source_frame": frame, "edge": edge, "native_frame": native_frame,
                            "difference": mismatch,
                        }
                        break
            events.append(check)

    source_active = sorted(source_save)
    source_parent_missing = sorted(set(range(source_stop)) - set(source_parent))
    native_active_source_frames = sorted(
        frame - native_offset for frame, row in native.items()
        if 0 <= frame - native_offset < source_stop and row[2] == 1
    )
    source_active_native_frames = sorted(source_active_set & set(native_active_source_frames))
    missing_native_manager_frames = sorted(source_active_set - set(native_active_source_frames))
    unexpected_native_manager_frames = sorted(set(native_active_source_frames) - source_active_set)
    native_interval = [frame for frame in native if native_offset <= frame < native_offset + source_stop]
    paired_value_equal = compared > 0 and not diffs
    same_input_replay = bool(anchor["input_match"])
    coverage_complete = (
        not missing_source and not missing_native and not source_parent_missing and
        not missing_native_manager_frames and not unexpected_native_manager_frames
    )
    report = {
        "schema": "okage-save-component-comparison-v1",
        "case": case,
        "scope": "Field mode 2 Save owner state at source callback entry compared with native completed-tick observations.",
        "alignment": {
            "native_offset": native_offset,
            "source_interval": [0, source_stop],
            "rule": "Fixed per-capture anchor only; source second_callback_entry at frame f maps to native completed tick f+offset, while source input_handler_entry maps to f+offset-1. No value fitting or later realignment.",
            "boundary": "The Save input-handler edge precedes the source second callback. The native trace is sampled after RestoreSession::tick_save has run both handlers; the distinct one-tick mapping preserves this callback order.",
            "source_owner": "Original parent_current40=2, linked Save manager type 8, selector 1f89a0=0.",
            "same_input_replay": same_input_replay,
            "input_caveat": (None if same_input_replay else
                             "The retained original-save-open-v1 pad is neutral and diverges from native-write inputs; mismatches are diagnostic only."),
        },
        "source": {
            "parent_frames": source_stop,
            "parent_missing_frames": source_parent_missing,
            "save_active_frames": len(source_active),
            "save_first_frame": source_active[0] if source_active else None,
            "save_last_frame": source_active[-1] if source_active else None,
            "frames_without_save_owner": sorted(set(range(source_stop)) - source_active_set),
            "callback_records_by_edge": {
                "input_handler_entry": sum("input_handler_entry" in v for v in source_save.values()),
                "second_callback_entry": sum("second_callback_entry" in v for v in source_save.values()),
            },
        },
        "native": {
            "record_count": len(native),
            "first_frame": min(native) if native else None,
            "last_frame": max(native) if native else None,
            "records_in_source_interval": len(native_interval),
            "save_manager_records_in_source_interval": sum(native[f][2] == 1 for f in native_interval),
            "active_source_frames_with_native_manager": source_active_native_frames,
            "source_active_frames_missing_native_manager": missing_native_manager_frames,
            "unexpected_native_manager_source_frames": unexpected_native_manager_frames,
            "native_preinterval_records_excluded": sum(frame < native_offset for frame in native),
        },
        "save_state": {
            "compared_records": compared,
            "compared_by_edge": dict(event_counts),
            "difference_words": sum(diffs.values()),
            "difference_counts_by_field": dict(sorted(diffs.items())),
            "callback_boundary_sensitive_difference_words": sum(boundary_diffs.values()),
            "callback_boundary_sensitive_difference_counts_by_field": dict(sorted(boundary_diffs.items())),
            "comparable_difference_words": sum(diffs.values()) - sum(boundary_diffs.values()),
            "comparable_difference_counts_by_field": dict(sorted((
                key, value - boundary_diffs[key]
            ) for key, value in diffs.items() if value - boundary_diffs[key])),
            "first_difference": first_difference,
            "first_comparable_difference": first_comparable_difference,
            "missing_source_count": len(missing_source),
            "missing_native_count": len(missing_native),
            "missing_source": missing_source,
            "missing_native": missing_native,
            "values_equal_when_paired": paired_value_equal,
            "coverage_complete": coverage_complete,
            "compared_fields": [name for name, _addr, _array_index, _idx in GLOBAL_FIELDS] +
                               [name for name, _offset in MANAGER_FIELDS] + ["dead"],
            "callback_boundary_policy": {
                "source_points": "Original records are callback entry snapshots; 0x0011e480 precedes the first handler and 0x0011c2c8 precedes the second handler.",
                "native_point": "Native records are sampled after both Save callbacks return.",
                "second_callback_entry_sensitive_fields": sorted(SECOND_CALLBACK_BOUNDARY_FIELDS),
                "meaning": "Raw differences remain in difference_words and checks. Differences on the listed fields at second_callback_entry are reported separately because the two samples straddle handler writes; they are not counted as comparable behavioral differences.",
            },
            "excluded": [
                {"field": "1f89a0 selector and definition pointers", "reason": "The native Save owner is selected by construction; its host record has no original EE address or selector field."},
                {"field": "storage_context, 2597a8/ac/b0 and 24ee storage globals", "reason": "Native filesystem worker state is not PS2 asynchronous storage-device state."},
                {"field": "window opacity and widget/RUW state", "reason": "This native record does not serialize the original callback-entry window state."},
                {"field": "manager_layer_state_cursor_e8c at input_handler_entry", "reason": "Original generic dispatch resets this cursor before the input-handler boundary; the native record is sampled after its completed input callback and retains the incremented value. The second_callback_entry value is compared."},
                {"field": "parent/request pointers, callback pointers, and manager/field addresses", "reason": "Host ownership addresses are not comparable to original EE pointers."},
                {"field": "input/controller snapshot", "reason": "Original input is sampled at the handler boundary, while the native record is after the completed callback pair."},
            ],
            "checks": events,
        },
        "storage_diagnostics": {
            "source_context_state_counts": {},
            "source_storage_global_value_counts": {},
            "native_io_state_counts": {},
            "native_success_tick_range": None,
            "native_slot_presence_error_counts": {},
            "note": "These are separate state summaries only; no host I/O state is equated with the original storage device or asynchronous command protocol.",
        },
        "paired_state_equal": same_input_replay and paired_value_equal and coverage_complete,
    }

    # Report storage progress separately from the semantic Save-state comparison.
    source_contexts = Counter()
    source_globals: dict[str, Counter[str]] = {}
    source_global_samples: dict[str, list[str]] = {}
    for frame in source_active:
        for edge_event in source_save[frame].values():
            context = edge_event["storage_context"]
            source_contexts["null" if context is None else
                            f"{context['control']:08x}/{context['pending']:08x}/{context['result']:08x}"] += 1
            for key, value in edge_event["storage_globals"].items():
                source_globals.setdefault(key, Counter())[value] += 1
                source_global_samples.setdefault(key, []).append(value)
    selected_native_rows = [native[f] for f in native_interval]
    io_counts = Counter(row[8] for row in selected_native_rows)
    success_ticks = [row[9] for row in selected_native_rows]
    slot_counts = Counter()
    for row in selected_native_rows:
        present = sum((row[index] != 0) << bit for bit, index in enumerate((14, 17, 20)))
        errors = sum((row[index] != 0) << bit for bit, index in enumerate((15, 18, 21)))
        slot_counts[f"present=0x{present:x},error=0x{errors:x}"] += 1
    report["storage_diagnostics"].update(
        source_context_state_counts=dict(sorted(source_contexts.items())),
        source_storage_global_summaries={
            key: value_summary(values) for key, values in sorted(source_global_samples.items())
        },
        native_io_state_counts={str(key): value for key, value in sorted(io_counts.items())},
        native_success_tick_range=([min(success_ticks), max(success_ticks)] if success_ticks else None),
        native_slot_presence_error_counts=dict(sorted(slot_counts.items())),
    )
    return report

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=sorted(ANCHORS), required=True)
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--native-save-state", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    if args.output.exists() or args.output.is_symlink():
        parser.error(f"Refusing existing output: {args.output}")
    anchor = ANCHORS[args.case]
    try:
        source_save, source_parent = load_source(args.source, anchor["source_stop"])
        native = load_native(args.native_save_state)
        report = compare(source_save, source_parent, native, case=args.case,
                         native_offset=anchor["native_offset"], source_stop=anchor["source_stop"])
        report["inputs"] = {
            "source_jsonl_sha256": sha256_file(args.source),
            "native_save_state_sha256": sha256_file(args.native_save_state),
            "source_elf_sha256": SOURCE_ELF_SHA256,
        }
        report["run_provenance"] = {
            "source": run_summary(args.source.parent / "run.json"),
            "native": run_summary(args.native_save_state.parent / "run.json"),
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except (ComparisonError, OSError) as exc:
        print(f"compare_save: {exc}", file=sys.stderr)
        return 2
    state = report["save_state"]
    print(f"case={args.case} compared={state['compared_records']} differences={state['difference_words']} "
          f"missing_native={state['missing_native_count']} output={args.output}")
    return 0 if report["paired_state_equal"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
