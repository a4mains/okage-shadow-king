#!/usr/bin/env python3
"""Compare native cold-start captures with original mode8 observations.

The scheduler comparison uses one fixed anchor: the first source global-parent
entry maps to native tick 1 point 0. Callback rows are compared to the native
state completed on the preceding tick. Missing source observations stay
missing; no state is held across observer gaps. Logo ordinal output is a
separate component diagnostic and is not global timing or raster parity.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from collections import Counter

SOURCE_SCHEMA = "okage-game-mode-observer-v1"
PARENT_PC = "0x00124818"
MODE8_CALLBACK_PC = "0x0012c8f8"
GAME_MODE_WORDS = 25
STARTUP_WORDS = 12
TITLE_WORDS = 43
FIXED_SOURCE_ANCHOR = 75

PARENT_FIELDS = (
    ("phase", "parent_phase20", 2),
    ("current_mode", "parent_current40", 3),
    ("arg1", "parent_arg44", 4),
    ("arg2", "parent_arg4c", 5),
    ("request_mode", "parent_request3c", 6),
    ("queued_arg1", "parent_queued48", 7),
    ("queued_arg2", "parent_queued50", 8),
    ("has_child", "parent_child38", 9),
    ("dead_linked", "parent_linked_first18", 10),
    ("pending_740", "pending_24f740", 12),
    ("pending_744", "pending_24f744", 13),
    ("pending_748", "pending_24f748", 14),
)

MODE8_FIELDS = (
    ("selector", "selector_24"),
    ("counter", "counter_30"),
    ("alpha_bits", "alpha_3c"),
    ("rate_bits", "rate_40"),
)

class ComparisonError(ValueError):
    pass

def _u32_hex(value: object, label: str) -> int:
    if not isinstance(value, str) or not value.startswith("0x"):
        raise ComparisonError(f"{label} must be a hexadecimal string")
    try:
        number = int(value, 16)
    except ValueError as exc:
        raise ComparisonError(f"{label} is invalid hexadecimal") from exc
    if not 0 <= number <= 0xFFFFFFFF:
        raise ComparisonError(f"{label} is outside u32 range")
    return number

def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

def _ranges(values: list[int] | set[int]) -> list[dict[str, int]]:
    ordered = sorted(set(values))
    if not ordered:
        return []
    ranges = []
    start = prior = ordered[0]
    for value in ordered[1:]:
        if value == prior + 1:
            prior = value
            continue
        ranges.append({"start": start, "end": prior, "count": prior - start + 1})
        start = prior = value
    ranges.append({"start": start, "end": prior, "count": prior - start + 1})
    return ranges

def load_source(path: Path) -> tuple[dict, list[dict], dict[int, dict], dict[int, dict]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise ComparisonError(f"cannot read source JSONL: {exc}") from exc
    if not lines:
        raise ComparisonError("source JSONL is empty")
    try:
        header = json.loads(lines[0])
    except json.JSONDecodeError as exc:
        raise ComparisonError(f"source header is invalid JSON: {exc}") from exc
    if not isinstance(header, dict) or header.get("schema") != SOURCE_SCHEMA:
        raise ComparisonError(f"source schema must be {SOURCE_SCHEMA}")

    all_rows: list[dict] = []
    parent: dict[int, dict] = {}
    callbacks: dict[int, dict] = {}
    last_parent = last_callback = -1
    for line_number, line in enumerate(lines[1:], 2):
        if not line.strip():
            raise ComparisonError(f"blank source record at line {line_number}")
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ComparisonError(f"invalid source JSON at line {line_number}: {exc}") from exc
        if not isinstance(row, dict):
            raise ComparisonError(f"source line {line_number} is not an object")
        all_rows.append(row)
        pc = row.get("pc")
        if pc == PARENT_PC:
            frame = row.get("frame")
            if not isinstance(frame, int) or isinstance(frame, bool) or frame < 0:
                raise ComparisonError(f"source parent line {line_number} has invalid frame")
            if frame in parent or frame <= last_parent:
                raise ComparisonError(f"source parent frames duplicate or out of order at {frame}")
            for _, field, _ in PARENT_FIELDS:
                _u32_hex(row.get(field), f"source line {line_number} {field}")
            parent[frame] = row
            last_parent = frame
        elif pc == MODE8_CALLBACK_PC:
            frame = row.get("frame")
            child = row.get("mode8_child")
            if not isinstance(frame, int) or isinstance(frame, bool) or frame < 0:
                raise ComparisonError(f"source mode8 callback line {line_number} has invalid frame")
            if frame in callbacks or frame <= last_callback:
                raise ComparisonError(f"source mode8 callback frames duplicate or out of order at {frame}")
            if not isinstance(child, dict):
                raise ComparisonError(f"source mode8 callback line {line_number} lacks mode8_child")
            for _, field in MODE8_FIELDS:
                _u32_hex(child.get(field), f"source line {line_number} mode8_child.{field}")
            _u32_hex(child.get("image_44"), f"source line {line_number} mode8_child.image_44")
            device = row.get("storage_device")
            if not isinstance(device, dict):
                raise ComparisonError(f"source mode8 callback line {line_number} lacks storage_device")
            for field in ("type_2597a8", "value_2597ac", "capacity_2597b0", "continuation_2597ca"):
                _u32_hex(device.get(field), f"source line {line_number} storage_device.{field}")
            callbacks[frame] = row
            last_callback = frame

    if not parent:
        raise ComparisonError("source has no global-parent entry rows at 0x00124818")
    return header, all_rows, parent, callbacks

def load_binary(path: Path, words: int, allow_empty: bool = False) -> list[tuple[int, ...]]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ComparisonError(f"cannot read {path}: {exc}") from exc
    if not data:
        if allow_empty:
            return []
        raise ComparisonError(f"{path} is empty")
    stride = words * 4
    if len(data) % stride:
        raise ComparisonError(f"{path} has {len(data)} bytes; expected {stride}-byte records")
    return list(struct.iter_unpack(f"<{words}I", data))

def _indexed_rows(rows: list[tuple[int, ...]], label: str, minimum_words: int,
                  frame_index: int = 0) -> dict[int, tuple[int, ...]]:
    indexed = {}
    previous = -1
    for i, row in enumerate(rows):
        if len(row) < minimum_words:
            raise ComparisonError(f"{label} row {i} is too short")
        frame = row[frame_index]
        if frame <= previous:
            raise ComparisonError(f"{label} frame order is not strictly increasing at {frame}")
        indexed[frame] = row
        previous = frame
    return indexed

def load_game_mode(path: Path) -> tuple[dict[int, tuple[int, ...]], list[tuple[int, ...]]]:
    rows = load_binary(path, GAME_MODE_WORDS)
    previous = (-1, -1)
    seen = set()
    points0 = {}
    for i, row in enumerate(rows):
        frame, point = row[:2]
        key = (frame, point)
        if point not in (0, 1):
            raise ComparisonError(f"native game-mode row {i} has invalid point {point}")
        if key in seen or key <= previous:
            raise ComparisonError(f"native game-mode frame/point order is invalid at {key}")
        seen.add(key)
        previous = key
        if point == 0:
            points0[frame] = row
    if not points0:
        raise ComparisonError("native game-mode capture has no point-0 rows")
    return points0, rows

def load_capture(prefix: Path) -> dict:
    game_path = Path(str(prefix) + ".game-mode.bin")
    startup_path = Path(str(prefix) + ".startup.bin")
    title_path = Path(str(prefix) + ".title-state.bin")
    metadata_path = Path(str(prefix) + ".json")
    if not title_path.exists() and not title_path.is_symlink():
        alternate = Path(str(prefix) + ".title.bin")
        if alternate.exists() or alternate.is_symlink():
            title_path = alternate
    game, game_all = load_game_mode(game_path)
    startup_rows = load_binary(startup_path, STARTUP_WORDS, allow_empty=True)
    title_rows = load_binary(title_path, TITLE_WORDS, allow_empty=True)
    startup = _indexed_rows(startup_rows, "native startup", STARTUP_WORDS)
    title = _indexed_rows(title_rows, "native title", TITLE_WORDS)
    if not metadata_path.is_file():
        raise ComparisonError(f"native capture frame-count metadata is missing: {metadata_path}")
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ComparisonError(f"cannot read native capture metadata: {exc}") from exc
    total_frames = metadata.get("frames") if isinstance(metadata, dict) else None
    if not isinstance(total_frames, int) or isinstance(total_frames, bool) or total_frames < 1:
        raise ComparisonError("native capture metadata must contain a positive integer frames field")
    if max(game) > total_frames or max(startup, default=0) > total_frames or max(title, default=0) > total_frames:
        raise ComparisonError("native trace contains a frame beyond capture metadata frames")
    if min(game) != 1:
        raise ComparisonError(f"native point-0 trace starts at tick {min(game)}; fixed anchor requires tick 1")
    return {
        "paths": {"game_mode": game_path, "startup": startup_path, "title": title_path,
                  "capture_metadata": metadata_path},
        "game_mode": game,
        "game_mode_all": game_all,
        "startup": startup,
        "title": title,
        "total_frames": total_frames,
    }

def _field_differences(expected: dict[str, int], actual: dict[str, int]) -> list[dict]:
    return [
        {"field": name, "source": f"0x{source:08x}", "native": f"0x{actual[name]:08x}"}
        for name, source in expected.items() if actual[name] != source
    ]

def _native_parent(row: tuple[int, ...]) -> dict[str, int]:
    values = {}
    for name, _, index in PARENT_FIELDS:
        value = row[index]
        if name in ("has_child", "dead_linked"):
            value = int(value != 0)
        values[name] = value
    return values

def _source_parent(row: dict) -> dict[str, int]:
    values = {}
    child = _u32_hex(row["parent_child38"], "parent_child38")
    linked = _u32_hex(row["parent_linked_first18"], "parent_linked_first18")
    for name, source_name, _ in PARENT_FIELDS:
        value = _u32_hex(row[source_name], source_name)
        if name == "has_child":
            value = int(child != 0)
        elif name == "dead_linked":
            value = int(linked != 0 and child == 0)
        values[name] = value
    return values

def _source_mode8(row: dict) -> dict[str, int]:
    child = row["mode8_child"]
    values = {name: _u32_hex(child[field], field) for name, field in MODE8_FIELDS}
    values["image_index"] = _u32_hex(child["image_44"], "image_44")
    device = row["storage_device"]
    values["source_storage_type"] = _u32_hex(device["type_2597a8"], "storage type")
    values["source_storage_value"] = _u32_hex(device["value_2597ac"], "storage value")
    values["source_free_capacity"] = _u32_hex(device["capacity_2597b0"], "storage capacity")
    values["storage_continuation"] = _u32_hex(device["continuation_2597ca"], "storage continuation")
    return values

def _native_startup(row: tuple[int, ...]) -> dict[str, int]:
    return {
        "selector": row[1], "counter": row[2], "alpha_bits": row[3],
        "rate_bits": row[4], "image_index": row[5],
        "storage_state": row[6], "storage_complete": row[7],
        "storage_type": row[8], "free_capacity": row[9],
    }

def _native_title(row: tuple[int, ...]) -> dict[str, int]:
    return {
        "selector": row[1], "counter": row[3], "alpha_bits": row[5],
        "rate_bits": row[6],
    }

def compare(source_header: dict, parents: dict[int, dict], callbacks: dict[int, dict],
            capture: dict, original_stop_boundary: int, boundary_source: str) -> dict:
    game_rows = capture["game_mode"]
    native_frames = sorted(game_rows)
    first_source = min(parents)
    first_native = native_frames[0]
    if first_native != 1:
        raise ComparisonError(f"native point-0 trace starts at tick {first_native}; fixed anchor requires tick 1")
    if first_source != FIXED_SOURCE_ANCHOR:
        raise ComparisonError(f"source first parent entry is frame {first_source}; this comparison requires fixed frame {FIXED_SOURCE_ANCHOR}")
    source_anchor = FIXED_SOURCE_ANCHOR
    native_anchor = 1
    # Fixed transform: source frame = source anchor + native frame - native anchor.
    offset = source_anchor - native_anchor
    original_last_frame = original_stop_boundary - 1
    if original_last_frame < source_anchor:
        raise ComparisonError("original stop boundary precedes first observed parent entry")
    capture_source_low = source_anchor
    capture_source_high = min(capture["total_frames"] + offset, original_last_frame)
    overlap_native_high = capture_source_high - offset
    if overlap_native_high < native_frames[0]:
        raise ComparisonError("native capture and requested original interval do not overlap")
    overlap_native_frames = [frame for frame in native_frames if frame <= overlap_native_high]
    out_of_overlap_native_frames = [frame for frame in native_frames if frame > overlap_native_high]
    mapped_source_frames = {frame + offset for frame in overlap_native_frames}
    compared_parent = []
    source_gaps = []
    counts: Counter[str] = Counter()
    first_parent_difference = None
    missing_source_parent = []
    source_only_parent = [frame for frame in parents
                          if frame < capture_source_low or frame > capture_source_high]
    missing_native_parent = []
    expected_source_frames = set(range(capture_source_low, capture_source_high + 1))
    for source_frame in sorted(expected_source_frames):
        frame = source_frame - offset
        original = parents.get(source_frame)
        if original is None:
            missing_source_parent.append(source_frame)
            continue
        if frame not in game_rows:
            continue
        source_values = _source_parent(original)
        native_values = _native_parent(game_rows[frame])
        differences = _field_differences(source_values, native_values)
        for difference in differences:
            counts[difference["field"]] += 1
        row = {
            "source_frame": source_frame,
            "native_frame": frame,
            "native_point": 0,
            "differences": differences,
            "equal": not differences,
        }
        compared_parent.append(row)
        if differences and first_parent_difference is None:
            first_parent_difference = {
                "source_frame": source_frame,
                "native_frame": frame,
                "difference": differences[0],
            }
    for frame in parents:
        if capture_source_low <= frame <= capture_source_high and frame not in mapped_source_frames:
            missing_native_parent.append(frame)

    native_missing_ticks = sorted(set(range(native_frames[0], overlap_native_high + 1)) - set(overlap_native_frames))
    source_only_parent = [frame for frame in parents
                          if capture_source_low <= frame <= original_last_frame and frame > capture_source_high]
    parent_channel = {
        "mapping": {
            "anchor_source_parent_entry_frame": source_anchor,
            "anchor_native_frame": native_anchor,
            "native_point": 0,
            "rule": "source_frame = source_anchor + native_frame - 1; no later alignment or fitting",
        },
        "source_parent_entries": len(parents),
        "native_point0_records": len(overlap_native_frames),
        "native_point0_records_observed": len(overlap_native_frames),
        "missing_native_point0_count": len(native_missing_ticks),
        "native_point0_records_outside_original_interval": len(out_of_overlap_native_frames),
        "native_frames_outside_original_interval": _ranges(out_of_overlap_native_frames),
        "compared_records": len(compared_parent),
        "missing_source_for_native_frames": len(missing_source_parent),
        "missing_source_frame_ranges": _ranges(missing_source_parent),
        "missing_native_point0_frame_ranges": _ranges(native_missing_ticks),
        "source_entries_without_native_point0": len(missing_native_parent),
        "source_entries_without_native_point0_ranges": _ranges(missing_native_parent),
        "source_entries_within_original_interval_outside_native_capture": len(source_only_parent),
        "source_only_frame_ranges": _ranges(source_only_parent),
        "difference_words": sum(counts.values()),
        "difference_counts_by_field": dict(sorted(counts.items())),
        "first_difference": first_parent_difference,
        "checks": compared_parent,
    }

    startup_source = {f: r for f, r in callbacks.items()
                      if _u32_hex(r["mode8_child"]["selector_24"], "selector") in (0, 3)
                      and f <= original_last_frame}
    title_source = {f: r for f, r in callbacks.items()
                    if _u32_hex(r["mode8_child"]["selector_24"], "selector") == 1
                    and f <= original_last_frame}
    startup_overlap = {f: row for f, row in capture["startup"].items()
                       if source_anchor + f <= original_last_frame}
    title_overlap = {f: row for f, row in capture["title"].items()
                     if source_anchor + f <= original_last_frame}
    startup_pairs, startup_missing_native, startup_missing_source = _compare_callback_channel(
        startup_source, startup_overlap, source_anchor, "startup")
    title_pairs, title_missing_native, title_missing_source = _compare_callback_channel(
        title_source, title_overlap, source_anchor, "title")
    startup_diff_counts, startup_first_difference = _callback_difference_summary(startup_pairs)
    title_diff_counts, title_first_difference = _callback_difference_summary(title_pairs)

    source_logo = [(frame, _source_mode8(row)) for frame, row in callbacks.items()
                   if _u32_hex(row["mode8_child"]["selector_24"], "selector") == 0
                   and frame <= original_last_frame]
    native_logo = [(frame, _native_startup(row)) for frame, row in capture["startup"].items()
                   if row[1] == 0 and source_anchor + frame <= original_last_frame]
    ordinal = []
    ordinal_counts: Counter[str] = Counter()
    ordinal_first_difference = None
    ordinal_source = source_logo[1:] if source_logo else []
    ordinal_native = native_logo[:-1] if native_logo else []
    for index, ((source_frame, source_values), (native_frame, native_values)) in enumerate(
            zip(ordinal_source, ordinal_native), start=1):
        compare_names = ("selector", "counter", "alpha_bits", "rate_bits", "image_index")
        expected = {name: source_values[name] for name in compare_names}
        actual = {name: native_values[name] for name in compare_names}
        differences = _field_differences(expected, actual)
        for difference in differences:
            ordinal_counts[difference["field"]] += 1
            if ordinal_first_difference is None:
                ordinal_first_difference = {"ordinal": index,
                                            "source_callback_frame": source_frame,
                                            "native_completed_frame": native_frame,
                                            "difference": difference}
        ordinal.append({"ordinal": index, "source_callback_frame": source_frame,
                        "native_completed_frame": native_frame, "differences": differences,
                        "equal": not differences})

    source_logo_frames = [frame for frame, _ in source_logo]
    logo_frame_gaps = [f for start, end in zip(source_logo_frames, source_logo_frames[1:])
                       for f in range(start + 1, end)]
    unsupported_callbacks = [
        {"source_frame": frame,
         "selector": _u32_hex(row["mode8_child"]["selector_24"], "selector")}
        for frame, row in callbacks.items()
        if _u32_hex(row["mode8_child"]["selector_24"], "selector") not in (0, 1, 3)
    ]

    parent_equal = not (missing_source_parent or native_missing_ticks or missing_native_parent or source_only_parent or
                        sum(counts.values()))
    callbacks_have_missing = bool(startup_missing_native or startup_missing_source or
                                  title_missing_native or title_missing_source)
    callbacks_have_diffs = any(not r["equal"] for r in startup_pairs + title_pairs)
    logo_has_diffs = any(not r["equal"] for r in ordinal)
    return {
        "schema": "okage-cold-start-comparison-v1",
        "scope": "Fixed-anchor scheduler and mode8 callback-state comparison.",
        "source": {"schema": source_header["schema"], "elf_sha256": source_header.get("elf_sha256"),
                   "parent_rows": len(parents), "mode8_callback_rows": len(callbacks)},
        "fixed_anchor_parent_entry": parent_channel,
        "original_interval": {"requested_vblanks_stop_boundary": original_stop_boundary,
                               "last_source_frame_in_interval": original_last_frame,
                               "boundary_provenance": boundary_source,
                               "native_capture_frames": capture["total_frames"],
                               "fixed_anchor_overlap_native_frames": [native_frames[0], overlap_native_high]},
        "mode8_callbacks": {
            "mapping": "source callback-entry frame maps to native completed tick source_frame - source_anchor; no interpolation or held values",
            "constructor_boundary": "Constructor-only source state is not synthesized. Native samples with no source callback at their fixed mapped source frame remain unpaired.",
            "source_callback_rows": len(callbacks),
            "startup_source_rows": len(startup_source),
            "title_source_rows": len(title_source),
            "startup": {"pairs": startup_pairs,
                        "difference_words": sum(startup_diff_counts.values()),
                        "difference_counts_by_field": dict(sorted(startup_diff_counts.items())),
                        "first_difference": startup_first_difference,
                        "missing_native_count": len(startup_missing_native),
                        "missing_native_source_frames": startup_missing_native,
                        "missing_source_count": len(startup_missing_source),
                        "missing_source_frame_ranges": _ranges(startup_missing_source),
                        "storage_capacity_direct_equality": "excluded; native host filesystem and original card capacities differ",
                        "storage_comparison": "source continuation word compared with native service-state word; device type/value and free capacity are retained, not asserted equal",
                        "storage_observations_retained": True},
            "title": {"pairs": title_pairs,
                      "difference_words": sum(title_diff_counts.values()),
                      "difference_counts_by_field": dict(sorted(title_diff_counts.items())),
                      "first_difference": title_first_difference,
                      "missing_native_count": len(title_missing_native),
                      "missing_native_source_frames": title_missing_native,
                      "missing_source_count": len(title_missing_source),
                      "missing_source_frame_ranges": _ranges(title_missing_source),
                      "unavailable_source_fields": ["title-specific image arrays", "pressed word", "yaw", "selection"],
                      "storage_fields": "not present in native title-state trace"},
            "unsupported_source_selectors": unsupported_callbacks,
            "source_logo_selector0_frame_gaps": _ranges(logo_frame_gaps),
            "ordinal_logo_progression": {
                "scope": "Ordinal component diagnostic only; not global timing/parity and not substituted for fixed-anchor results.",
                "boundary": "source callback-entry counter 0 has no native prior completed selector-0 sample; then source counters 1..N pair by ordinal with native completed counters 1..N; final native completion has no later source entry",
                "source_samples": len(source_logo), "native_samples": len(native_logo),
                "paired_samples": len(ordinal),
                "unpaired_source_samples": max(0, len(source_logo) - len(ordinal)),
                "unpaired_native_samples": max(0, len(native_logo) - len(ordinal)),
                "unpaired_source_endpoints": ([{"frame": source_logo[0][0], "counter": source_logo[0][1]["counter"],
                                                 "reason": "selector-0 constructor entry has no native prior completed tick"}]
                                               if source_logo else []),
                "unpaired_native_endpoints": ([{"frame": native_logo[-1][0], "counter": native_logo[-1][1]["counter"],
                                                 "reason": "final native completion has no subsequent source callback entry"}]
                                               if native_logo else []),
                "difference_counts_by_field": dict(sorted(ordinal_counts.items())),
                "first_difference": ordinal_first_difference,
                "paired_progression_words_equal": not ordinal_counts,
                "pairs": ordinal,
            },
        },
        "storage_note": "Original device type/value/capacity/continuation and native state/completion/type/free-capacity are retained per paired startup callback. Only source continuation versus native state is compared; free-capacity words are not directly compared.",
        "limitations": [
            "No original raster or audio normalization/parity is attempted.",
            "Source callback gaps are unobserved intervals, not held prior values.",
            "Source manager/HERO/audio side effects and constructor-only state are outside these traces.",
        ],
        "semantic_equal": parent_equal and not callbacks_have_missing and not callbacks_have_diffs,
        "ordinal_logo_equal": not logo_has_diffs and len(source_logo) == len(native_logo),
    }

def _compare_callback_channel(source_rows: dict[int, dict], native_rows: dict[int, tuple[int, ...]],
                              source_anchor: int, channel: str) -> tuple[list[dict], list[int], list[int]]:
    paired = []
    missing_native = []
    paired_native_frames = set()
    for source_frame, row in source_rows.items():
        native_frame = source_frame - source_anchor
        native = native_rows.get(native_frame)
        if native is None:
            missing_native.append(source_frame)
            continue
        paired_native_frames.add(native_frame)
        source_values = _source_mode8(row)
        if channel == "startup":
            native_values = _native_startup(native)
            names = ("selector", "counter", "alpha_bits", "rate_bits", "image_index", "storage_state")
            # The source continuation and native storage-state words share the
            # observed 0x687/0x69a continuation values.
            source_values["storage_state"] = source_values["storage_continuation"]
        else:
            native_values = _native_title(native)
            names = ("selector", "counter", "alpha_bits", "rate_bits")
        expected = {name: source_values[name] for name in names}
        actual = {name: native_values[name] for name in names}
        differences = _field_differences(expected, actual)
        storage = None
        if channel == "startup":
            storage = {"source": {"device_type": source_values["source_storage_type"],
                                   "device_value": source_values["source_storage_value"],
                                   "free_capacity": source_values["source_free_capacity"],
                                   "continuation": source_values["storage_continuation"]},
                       "native": {"state": native_values["storage_state"],
                                  "complete": native_values["storage_complete"],
                                  "device_type": native_values["storage_type"],
                                  "free_capacity": native_values["free_capacity"]}}
        paired.append({"source_callback_frame": source_frame,
                       "native_completed_frame": native_frame,
                       "source_boundary": "callback entry",
                       "native_boundary": "completed previous tick",
                       "differences": differences,
                       "storage_observations": storage,
                       "equal": not differences})
    missing_source = sorted(source_anchor + frame for frame in native_rows
                            if source_anchor + frame not in source_rows)
    return paired, sorted(missing_native), missing_source

def _callback_difference_summary(pairs: list[dict]) -> tuple[Counter[str], dict | None]:
    counts: Counter[str] = Counter()
    first = None
    for pair in pairs:
        for difference in pair["differences"]:
            counts[difference["field"]] += 1
            if first is None:
                first = {"source_callback_frame": pair["source_callback_frame"],
                         "native_completed_frame": pair["native_completed_frame"],
                         "difference": difference}
    return counts, first

def _stop_boundary_from_run(reference: Path, override: int | None) -> tuple[int, str, Path | None]:
    run_path = reference.parent / "run.json"
    inferred = None
    if run_path.exists():
        try:
            run = json.loads(run_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise ComparisonError(f"cannot read original run.json: {exc}") from exc
        command = run.get("command") if isinstance(run, dict) else None
        if isinstance(command, list) and all(isinstance(value, str) for value in command):
            try:
                option = command.index("--game-mode")
            except ValueError:
                option = -1
            if option > 0 and option + 1 < len(command):
                target = Path(command[option + 1])
                if target.name == reference.name and target.parent.name == reference.parent.name:
                    numbers = [int(value) for value in command[2:option]
                               if value.isdecimal()]
                    if numbers:
                        inferred = numbers[-1]
    if override is not None:
        if not isinstance(override, int) or isinstance(override, bool) or override <= 0:
            raise ComparisonError("original stop boundary must be a positive VBlank count")
        source = "explicit --original-stop-boundary"
        if inferred is not None:
            source += f"; run.json requested {inferred}"
        return override, source, run_path if run_path.exists() else None
    if inferred is None:
        raise ComparisonError("cannot infer original VBlank stop boundary; provide --original-stop-boundary")
    return inferred, f"{run_path} command line; requested {inferred} VBlanks", run_path

def make_report(reference: Path, prefix: Path,
                original_stop_boundary: int | None = None) -> tuple[dict, dict[str, Path]]:
    header, _, parents, callbacks = load_source(reference)
    capture = load_capture(prefix)
    stop_boundary, boundary_source, run_path = _stop_boundary_from_run(reference, original_stop_boundary)
    report = compare(header, parents, callbacks, capture, stop_boundary, boundary_source)
    paths = {"source": reference, **capture["paths"]}
    if run_path is not None:
        paths["source_run_json"] = run_path
    report["inputs"] = {
        key: {"path": str(path), "sha256": _sha256(path)}
        for key, path in paths.items()
    }
    report["inputs"]["source"]["parent_frame_range"] = [min(parents), max(parents)]
    report["inputs"]["source"]["mode8_callback_frame_range"] = [min(callbacks), max(callbacks)] if callbacks else None
    return report, paths

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True, type=Path, help="Original mode8 observer JSONL")
    parser.add_argument("--native-prefix", required=True, type=Path,
                        help="Capture prefix; reads .game-mode.bin, .startup.bin, and .title-state.bin")
    parser.add_argument("--original-stop-boundary", type=int,
                        help="Requested source VBlank stop count if sibling run.json cannot establish it")
    parser.add_argument("--output", required=True, type=Path, help="New JSON report path")
    args = parser.parse_args(argv)
    if args.output.exists() or args.output.is_symlink():
        parser.error(f"refusing existing output: {args.output}")
    try:
        report, _ = make_report(args.reference, args.native_prefix, args.original_stop_boundary)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except (ComparisonError, OSError) as exc:
        print(f"compare_cold_start: {exc}", file=sys.stderr)
        return 2
    print(f"parent compared={report['fixed_anchor_parent_entry']['compared_records']} "
          f"differences={report['fixed_anchor_parent_entry']['difference_words']} "
          f"mode8 startup/title pairs={len(report['mode8_callbacks']['startup']['pairs'])}/"
          f"{len(report['mode8_callbacks']['title']['pairs'])} "
          f"equal={str(report['semantic_equal']).lower()} output={args.output}")
    return 0 if report["semantic_equal"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
