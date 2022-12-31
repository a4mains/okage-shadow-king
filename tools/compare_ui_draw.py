#!/usr/bin/env python3
"""Compare source-observed logical UI packet replays and projection to native draw IR."""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from pathlib import Path

REFERENCE_SCHEMA = "okage-ui-draw-submission-v1"
NATIVE_SCHEMA = "okage-native-ui-draw-list-v1"
SOURCE_ELF_SHA256 = "2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5"
MAX_LINES = 25000
MAX_BYTES = 64 * 1024 * 1024

class Invalid(ValueError):
    pass

def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise Invalid(f"duplicate JSON key {key!r}")
        result[key] = value
    return result

def _uint(value, name, maximum=0xffffffff):
    if type(value) is not int or value < 0 or value > maximum:
        raise Invalid(f"{name} must be an unsigned integer in range")
    return value

def _hex(value, size, name):
    if not isinstance(value, str) or len(value) != size * 2:
        raise Invalid(f"{name} must contain exactly {size} bytes of hex")
    try:
        decoded = bytes.fromhex(value)
        if decoded.hex() != value.lower():
            raise ValueError("non-canonical hex")
        return decoded
    except ValueError as exc:
        raise Invalid(f"{name} is malformed hex") from exc

def _addr(value, name):
    if not isinstance(value, str):
        raise Invalid(f"{name} must be a quoted hexadecimal address")
    try:
        parsed = int(value, 0)
    except ValueError as exc:
        raise Invalid(f"{name} is malformed hexadecimal address") from exc
    if parsed < 0 or parsed > 0xffffffff:
        raise Invalid(f"{name} is out of range")
    return f"{parsed:08x}"

def _float_words(raw_little_endian):
    if len(raw_little_endian) % 4:
        raise Invalid("float payload is not word aligned")
    return b"".join(struct.pack(">I", word[0]) for word in struct.iter_unpack("<I", raw_little_endian))

def load_reference(path: Path):
    data = path.read_bytes()
    if not data or len(data) > MAX_BYTES:
        raise Invalid("reference size is empty or exceeds bound")
    try:
        lines = data.decode("utf-8").splitlines()
        rows = [json.loads(line, object_pairs_hook=_unique_object) for line in lines if line.strip()]
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise Invalid(f"reference JSONL is malformed: {exc}") from exc
    if len(rows) > MAX_LINES or not rows or type(rows[0]) is not dict:
        raise Invalid("reference row count/header is invalid")
    header = rows[0]
    if header.get("schema") != REFERENCE_SCHEMA:
        raise Invalid("unsupported reference schema")
    if header.get("source_elf_sha256") != SOURCE_ELF_SHA256:
        raise Invalid("reference source ELF does not match the pinned retail image")
    if header.get("scope") != "CPU submission calls inside the first source-gated 1a8150 draw tail-call; no deferred GS vertices or raster output":
        raise Invalid("unexpected reference scope")
    events = rows[1:]
    if not events or any(type(row) is not dict for row in events):
        raise Invalid("reference events are missing or malformed")
    finishes = [row for row in events if row.get("event") == "finish"]
    entries = [row for row in events if row.get("event") == "draw_submission_entry"]
    completions = [row for row in events if row.get("event") == "draw_dispatch_completion"]
    if len(finishes) != 1 or len(entries) != 1 or len(completions) != 1:
        raise Invalid("reference requires one draw entry, completion, and finish")
    if events[-1] is not finishes[0] or events.index(completions[0]) >= events.index(finishes[0]):
        raise Invalid("reference completion/finish order is invalid")
    completion_position = events.index(completions[0])
    finish = finishes[0]
    if finish.get("draw_completed") is not True:
        raise Invalid("reference draw is incomplete")
    draw_entry, completion = entries[0], completions[0]
    if events[0] is not draw_entry:
        raise Invalid("reference helpers precede draw entry")
    frame = _uint(draw_entry.get("frame"), "reference draw frame")
    if _uint(completion.get("frame"), "reference completion frame") != frame:
        raise Invalid("reference draw entry/completion frames differ")
    if completion.get("shared_return") != draw_entry.get("ra") or completion.get("sp") != draw_entry.get("sp"):
        raise Invalid("reference draw continuation does not pair")
    if draw_entry.get("ra") != completion.get("ra") or draw_entry.get("root") != completion.get("root") or draw_entry.get("manager") != completion.get("manager"):
        raise Invalid("reference draw owner/continuation changed")
    windows = draw_entry.get("windows")
    if type(windows) is not list or len(windows) != 2 or [w.get("slot") for w in windows if type(w) is dict] != [0, 1]:
        raise Invalid("reference must identify both selected windows")
    for window in windows:
        flags = _uint(window.get("flags_4f8"), "source window draw flags")
        if flags & 1:
            raise Invalid("source history is active and outside this comparison scope")

    calls = {}
    returned = set()
    call_events = []
    aliases = []
    alias_ids = set()
    call_stack = []
    allowed = {"draw_submission_entry", "call_entry", "call_return", "thunk_alias_entry", "draw_dispatch_completion", "finish"}
    for event_index, row in enumerate(events):
        event = row.get("event")
        if event not in allowed:
            raise Invalid(f"unknown reference event {event!r}")
        if event in ("call_entry", "call_return"):
            if event_index >= completion_position or row.get("frame") != frame:
                raise Invalid("helper event is outside the aligned source draw boundary")
            cid = _uint(row.get("call_id"), "call_id")
            if event == "call_entry":
                if cid in calls:
                    raise Invalid("duplicate helper call entry")
                if cid != len(calls) + 1:
                    raise Invalid("helper call identifiers are not strictly sequential")
                _uint(row.get("frame"), "helper frame")
                _uint(row.get("pc"), "helper PC")
                _uint(row.get("ra"), "helper RA")
                _uint(row.get("sp"), "helper SP")
                calls[cid] = row
                call_stack.append(cid)
                call_events.append(row)
            else:
                entry = calls.get(cid)
                if entry is None or _addr(entry.get("callee"), "entry callee") != _addr(row.get("callee"), "return callee") or entry.get("ra") != row.get("ra") or entry.get("sp") != row.get("sp"):
                    raise Invalid("helper return lacks matching entry identity")
                if cid in returned:
                    raise Invalid("duplicate helper return")
                if not call_stack or call_stack[-1] != cid:
                    raise Invalid("helper returns are not properly nested")
                call_stack.pop()
                returned.add(cid)
        elif event == "thunk_alias_entry":
            if event_index >= completion_position or row.get("frame") != frame:
                raise Invalid("thunk alias is outside the aligned source draw boundary")
            cid = _uint(row.get("call_id"), "alias call_id")
            _uint(row.get("frame"), "alias frame")
            _uint(row.get("pc"), "alias PC")
            _uint(row.get("ra"), "alias RA")
            _uint(row.get("sp"), "alias SP")
            if _addr(row.get("aliases"), "alias target") != "001ad3c0":
                raise Invalid("unknown helper thunk alias")
            if cid in alias_ids or not call_stack or call_stack[-1] != cid:
                raise Invalid("thunk alias does not occur once inside its active call")
            alias_ids.add(cid)
            aliases.append((cid, row))
    if set(calls) != returned:
        raise Invalid("reference helper calls are unbalanced")
    if call_stack:
        raise Invalid("reference helper stack is not empty at completion")
    if any(_uint(row.get("helper_calls"), "reported source helper count") != len(calls)
           for row in (completion, finish)):
        raise Invalid("reference helper totals disagree with observed pairs")
    for cid, alias in aliases:
        source = calls.get(cid)
        if source is None or _addr(source.get("callee"), "source callee") != "001ad3c0" or source.get("frame") != alias.get("frame") or source.get("sp") != alias.get("sp") or source.get("ra") != alias.get("ra"):
            raise Invalid("thunk alias does not identify its active packet replay")

    # Validate required source payloads and normalize guest addresses away.
    by_callee = lambda target: [row for row in call_events if _addr(row.get("callee"), "helper callee") == target]
    projection_rows = by_callee("001ad850")
    matrix_rows = by_callee("001ad3c8")
    color_rows = by_callee("001aeb48")
    gs_rows = by_callee("001aedd8")
    auxiliary_uploads = []
    auxiliary_by_call = {}
    if len(projection_rows) != 1:
        raise Invalid("reference must contain exactly one outer projection upload")
    projection = _float_words(_hex(projection_rows[0].get("outer_projection_vectors", {}).get("raw_hex"), 128, "outer projection"))
    matrices = [_float_words(_hex(row.get("vector_upload", {}).get("raw_hex"), 64, "matrix upload")) for row in matrix_rows]
    colors = [_float_words(_hex(row.get("selected_element_color_row", {}).get("raw_hex"), 16, "selected element color")) for row in color_rows]
    for i, row in enumerate(call_events):
        if _addr(row.get("callee"), "helper callee") != "001aeb10":
            continue
        count = _uint(row.get("a1"), "auxiliary vector count", 2)
        site = _addr(row.get("callsite"), "auxiliary upload callsite")
        if count == 1:
            expected_sites = ["001a85b0"]
        elif count == 2:
            expected_sites = ["001a8bc8", "001a8bdc"]
        else:
            raise Invalid("source auxiliary upload has unsupported vector count")
        following = call_events[i + 1:i + 1 + count]
        if len(following) != count or [_addr(x.get("callee"), "auxiliary value callee") for x in following] != ["001ae880"] * count:
            raise Invalid("auxiliary vector payload calls do not immediately follow their aeb10 selector")
        if site not in ("001a859c", "001a8bb4") or [_addr(x.get("callsite"), "auxiliary value callsite") for x in following] != expected_sites:
            raise Invalid("source auxiliary callsites/order are unsupported")
        grouped = []
        for value_call in following:
            lanes = value_call.get("f12_f15_bits")
            if type(lanes) is not list or len(lanes) != 4:
                raise Invalid("source auxiliary payload lacks F12-F15")
            value = b"".join(struct.pack(">I", _uint(x, "source auxiliary float bits")) for x in lanes)
            auxiliary_uploads.append(value)
            grouped.append(value.hex())
        auxiliary_by_call[_uint(row.get("call_id"), "auxiliary call id")] = grouped
    gs = []
    for row in gs_rows:
        raw_value = row.get("gs_value_a1_u64")
        if not isinstance(raw_value, str) or not raw_value.startswith("0x") or len(raw_value) != 18:
            raise Invalid("source GS value must be 64-bit prefixed hex")
        try:
            value = int(raw_value, 16)
        except ValueError as exc:
            raise Invalid("source GS value is malformed") from exc
        gs.append((_uint(row.get("gs_register"), "source GS register", 0xff), value))
    replays = []
    replay_by_call = {}
    for row in call_events:
        callee = _addr(row.get("callee"), "replay callee")
        if callee not in ("001ad3c0", "001c9368"):
            continue
        owner = row.get("packet_owner")
        if not isinstance(owner, dict):
            raise Invalid("packet replay entry lacks owner metadata")
        family = owner.get("draw_type")
        if family not in ("glyph_pass", "sprite_header", "sprite_element"):
            raise Invalid("unknown source replay draw type")
        owner_family = owner.get("family")
        if owner_family != ("glyph" if family == "glyph_pass" else "sprite"):
            raise Invalid("source replay family/type disagree")
        replay = {"family": family,
            "window": _uint(owner.get("window_slot"), "source replay window", 7),
            "group": _uint(owner.get("group"), "source replay group"),
            "side": _uint(owner.get("side"), "source replay side", 1),
            "element": None if owner_family == "glyph" or owner.get("element") is None else _uint(owner.get("element"), "source replay element", 15),
            "history": 0,
            "pass": _uint(owner.get("pass_index"), "source replay pass", 2),
        }
        replays.append(replay)
        replay_by_call[_uint(row.get("call_id"), "replay call id")] = replay
    if not replays:
        raise Invalid("reference contains no logical packet replay entries")
    projection_by_call = {_uint(row.get("call_id"), "projection call id"): projection.hex() for row in projection_rows}
    matrix_by_call = {_uint(row.get("call_id"), "matrix call id"): value.hex() for row, value in zip(matrix_rows, matrices)}
    color_by_call = {_uint(row.get("call_id"), "color call id"): value.hex() for row, value in zip(color_rows, colors)}
    gs_by_call = {_uint(row.get("call_id"), "GS call id"): pair for row, pair in zip(gs_rows, gs)}
    stream = []
    for row in call_events:
        cid = _uint(row.get("call_id"), "stream call id")
        callee = _addr(row.get("callee"), "stream helper callee")
        if cid in projection_by_call:
            stream.append({"kind": "projection", "bits": projection_by_call[cid]})
        elif cid in matrix_by_call:
            stream.append({"kind": "matrix", "bits": matrix_by_call[cid]})
        elif cid in auxiliary_by_call:
            stream.append({"kind": "auxiliary", "vectors": auxiliary_by_call[cid]})
        elif cid in color_by_call:
            stream.append({"kind": "colors", "bits": color_by_call[cid]})
        elif cid in gs_by_call:
            register, value = gs_by_call[cid]
            stream.append({"kind": "gs_register", "register": register, "value": value})
        elif cid in replay_by_call:
            stream.append({"kind": "replay", "owner": replay_by_call[cid]})
    return {"frame": frame, "projection": projection.hex(), "matrices": [x.hex() for x in matrices],
            "colors": [x.hex() for x in colors], "auxiliary": [x.hex() for x in auxiliary_uploads], "gs": gs, "replays": replays,
            "stream": stream,
            "helper_calls": len(call_events), "aliases": len(aliases),
            "source_scope": header["scope"]}

def load_native(path: Path):
    data = path.read_bytes()
    if not data or len(data) > MAX_BYTES:
        raise Invalid("native report size is empty or exceeds bound")
    try:
        report = json.loads(data, object_pairs_hook=_unique_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise Invalid(f"native JSON is malformed: {exc}") from exc
    if type(report) is not dict or report.get("schema") != NATIVE_SCHEMA:
        raise Invalid("unsupported native schema")
    if report.get("scope") != "ordered native UiDrawList snapshot; no GPU submission, GS execution, vertices, or raster equivalence":
        raise Invalid("unexpected native scope")
    if report.get("execution") != "actual-session-first-resource-after-callbacks":
        raise Invalid("native execution is not the aligned actual-session draw")
    frame = _uint(report.get("frame"), "native frame")
    input_anchor = _uint(report.get("input_anchor"), "native input anchor")
    reference_vblank = _uint(report.get("reference_vblank"), "native reference vblank")
    commands = report.get("commands")
    if type(commands) is not list or not commands or len(commands) > MAX_LINES:
        raise Invalid("native command list missing or out of bounds")
    if _uint(report.get("command_count"), "native command_count") != len(commands):
        raise Invalid("native command_count disagrees with command list")
    projections, matrices, colors, auxiliary, gs, replays = [], [], [], [], [], []
    stream = []
    packet_operation_count = 0
    allowed_kinds = {"control", "gs_register", "current_framebuffer", "projection", "matrix", "vectors", "replay"}
    for index, command in enumerate(commands):
        if type(command) is not dict or _uint(command.get("index"), "native command index") != index:
            raise Invalid("native command indices are malformed or nonmonotonic")
        kind = command.get("kind")
        if kind not in allowed_kinds:
            raise Invalid("native command kind is unsupported")
        if kind == "control":
            if command.get("name") not in {"begin", "cull_mode_zero", "begin_gif", "end_gif", "flush_vu", "end", "queue", "queue_text_state"}:
                raise Invalid("native control command is unknown")
        elif kind == "current_framebuffer":
            if command.get("target") != "runtime_current":
                raise Invalid("native framebuffer target is not runtime resolved")
        elif kind == "projection":
            words = command.get("float_bits")
            if type(words) is not list or len(words) != 32:
                raise Invalid("native projection extent is not 32 words")
            projections.append(b"".join(_hex(x, 4, "native projection word") for x in words))
            stream.append({"kind": "projection", "bits": projections[-1].hex()})
        elif kind == "matrix":
            words = command.get("float_bits")
            if type(words) is not list or len(words) != 16:
                raise Invalid("native matrix extent is not 16 words")
            matrices.append(b"".join(_hex(x, 4, "native matrix word") for x in words))
            stream.append({"kind": "matrix", "bits": matrices[-1].hex()})
        elif kind == "vectors":
            if command.get("vector_kind") not in ("colors", "auxiliary"):
                raise Invalid("native vector kind is unknown")
            values = command.get("values_f32_bits")
            if type(values) is not list or not values or len(values) > 32:
                raise Invalid("native vector upload is empty or exceeds bound")
            for lanes in values:
                if type(lanes) is not list or len(lanes) != 4:
                    raise Invalid("native vector lane extent is invalid")
                for lane in lanes:
                    _hex(lane, 4, "native vector lane")
            if command["vector_kind"] == "auxiliary":
                grouped = [b"".join(_hex(x, 4, "native auxiliary lane") for x in lanes) for lanes in values]
                auxiliary.extend(grouped)
                stream.append({"kind": "auxiliary", "vectors": [x.hex() for x in grouped]})
            else:
                if len(values) != 1:
                    raise Invalid("native color upload must contain one exact vector")
                colors.append(b"".join(_hex(x, 4, "native color lane") for x in values[0]))
                stream.append({"kind": "colors", "bits": colors[-1].hex()})
        elif kind == "gs_register":
            register = _uint(command.get("register"), "native GS register", 0xff)
            value = command.get("value_u64")
            if not isinstance(value, str) or len(value) != 16:
                raise Invalid("native GS value must be 64-bit hex")
            gs.append((register, int.from_bytes(_hex(value, 8, "native GS value"), "big")))
            stream.append({"kind": "gs_register", "register": gs[-1][0], "value": gs[-1][1]})
        elif kind == "replay":
            owner = command.get("owner")
            if type(owner) is not dict:
                raise Invalid("native replay owner missing")
            family = owner.get("family")
            if family not in ("glyph", "sprite_header", "sprite_element"):
                raise Invalid("native replay family unsupported")
            packet = command.get("packet")
            if type(packet) is not dict or type(packet.get("operations")) is not list or type(packet.get("elements")) is not list or len(packet["elements"]) != 16:
                raise Invalid("native replay packet shape is invalid")
            for operation_list in [packet["operations"], *(x for x in packet["elements"] if x is not None)]:
                if type(operation_list) is not list:
                    raise Invalid("native packet element is not an operation list")
                packet_operation_count += len(operation_list)
                if packet_operation_count > 200000:
                    raise Invalid("native packet operation count exceeds bound")
                for operation in operation_list:
                    if type(operation) is not dict or operation.get("kind") not in {"positions", "colors", "uv", "register", "texture", "primitive", "indexed_vertex", "compiler_boundary", "auxiliary_vectors"}:
                        raise Invalid("native packet operation is invalid")
                    _hex(operation.get("source_helper"), 4, "native packet source helper")
                    arguments = operation.get("arguments_u64")
                    if type(arguments) is not list or len(arguments) != 4:
                        raise Invalid("native packet argument extent is invalid")
                    for argument in arguments:
                        _hex(argument, 8, "native packet argument")
                    values = operation.get("values_f32_bits")
                    if type(values) is not list or len(values) > 100000:
                        raise Invalid("native packet values extent is invalid")
                    for vector in values:
                        if type(vector) is not list or len(vector) != 4:
                            raise Invalid("native packet vector extent is invalid")
                        for lane in vector:
                            _hex(lane, 4, "native packet float lane")
                    marker = operation.get("first_positions_upload")
                    if marker is not None and type(marker) is not bool:
                        raise Invalid("native packet positions marker is invalid")
            replay = {"family": family, "window": _uint(owner.get("window"), "native replay window", 7),
                "group": _uint(owner.get("group"), "native replay group"),
                "side": _uint(owner.get("side"), "native replay side", 1),
                "element": None if family == "glyph" else _uint(owner.get("element"), "native replay element", 15),
                "history": _uint(owner.get("history"), "native replay history", 15),
                "pass": _uint(owner.get("pass"), "native replay pass", 2)}
            replays.append(replay)
            stream.append({"kind": "replay", "owner": replay})
    if len(projections) != 1:
        raise Invalid("native list must contain exactly one projection")
    if _uint(report.get("packet_operation_count"), "native packet_operation_count") != packet_operation_count:
        raise Invalid("native packet_operation_count disagrees with packet snapshots")
    return {"frame": frame, "input_anchor": input_anchor, "reference_vblank": reference_vblank,
            "projection": projections[0].hex(), "matrices": [x.hex() for x in matrices],
            "colors": [x.hex() for x in colors], "auxiliary": [x.hex() for x in auxiliary], "gs": gs, "replays": replays,
            "stream": stream}

def compare(reference, native, input_anchor, original_vblank):
    if input_anchor != 104376 or original_vblank != 127:
        raise Invalid("this comparator is pinned to input anchor 104376 and original draw vblank 127")
    if reference["frame"] != original_vblank:
        raise Invalid("--original-vblank must equal the observed original draw frame")
    if native["input_anchor"] != input_anchor or native["reference_vblank"] != original_vblank:
        raise Invalid("native report does not attest to the explicit input/original-vblank alignment")
    differences = []
    if reference["projection"] != native["projection"]:
        differences.append({"field": "projection.float_bits", "expected": reference["projection"], "actual": native["projection"]})
    for field in ("matrices", "colors", "auxiliary", "gs", "replays", "stream"):
        expected, actual = reference[field], native[field]
        if expected != actual:
            first = next((i for i,(a,b) in enumerate(zip(expected,actual)) if a != b), min(len(expected),len(actual)))
            differences.append({"field": field, "expected_count": len(expected), "actual_count": len(actual),
                "first_index": first, "expected": expected[first] if first < len(expected) else None,
                "actual": actual[first] if first < len(actual) else None})
    return differences

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--input-anchor", type=lambda value: int(value, 0), required=True,
                        help="explicit input interval anchor; no frame shifting is performed")
    parser.add_argument("--original-vblank", type=lambda value: int(value, 0), required=True,
                        help="observed original draw vblank, e.g. 127")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.output.exists() or args.output.is_symlink():
            raise Invalid("refusing existing output")
        reference = load_reference(args.reference)
        native = load_native(args.native)
        differences = compare(reference, native, args.input_anchor, args.original_vblank)
        report = {"schema": "okage-ui-draw-comparison-v1", "input_anchor": args.input_anchor,
            "original_vblank": args.original_vblank,
            "original_frame": reference["frame"], "native_frame": native["frame"],
            "native_execution": "actual-session-first-resource-after-callbacks",
            "scope": "exact source/native comparison of projection bits, per-window matrices, selected color uploads, direct GS register writes, and logical replay owner order",
            "compared_counts": {"projection_words": 32, "matrices": len(reference["matrices"]),
                "color_vectors": len(reference["colors"]), "auxiliary_vectors": len(reference["auxiliary"]),
                "gs_registers": len(reference["gs"]),
                "logical_replays": len(reference["replays"]), "source_helper_pairs": reference["helper_calls"],
                "source_alias_entries_not_double_counted": reference["aliases"]},
            "differences": differences, "difference_count": len(differences),
            "exact_on_compared_channels": not differences,
            "unavailable_or_unsupported": ["source replay packet payload bytes are not captured; packet payloads are not compared",
                "current-framebuffer binding and control commands are not source-compared",
                "trailing queue_text_state packet bytes are not captured by the source observer",
                "no deferred GS packet execution, VU vertices, GPU submission, or raster equivalence"]}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2, allow_nan=False)
            stream.write("\n")
        print(json.dumps({"report": str(args.output), "difference_count": len(differences)}))
        return 0 if not differences else 1
    except (Invalid, OSError, TypeError, KeyError, struct.error, ValueError) as exc:
        print(f"compare_ui_draw: {exc}", file=sys.stderr)
        return 2

if __name__ == "__main__":
    raise SystemExit(main())
