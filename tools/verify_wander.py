#!/usr/bin/env python3
"""Compare paired original MEG callbacks with native variable-record samples at fixed ticks."""
import argparse
import hashlib
import json
import struct
from collections import Counter, defaultdict
from pathlib import Path

SCHEMA = "okage-meg-wander-pairs"
ENTRY_PC = 0x137DF8
EXIT_PC = 0x138020
TARGET_SCENE = 68
TARGET_ALIAS = -13
MAX_CHANNELS = 5120
NATIVE_HEADER_WORDS = 11

class WanderFormatError(ValueError):
    pass

def _u32(value, where):
    if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= 0xFFFFFFFF:
        raise WanderFormatError(f"{where} must be an unsigned 32-bit integer")
    return value

def _hash(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()

def load_original(path, requested_ticks):
    snapshots = []
    header = None
    try:
        lines = path.read_text().splitlines()
    except OSError as error:
        raise WanderFormatError(f"cannot read original JSONL: {error}") from error
    if not lines:
        raise WanderFormatError("original JSONL is empty")
    try:
        header = json.loads(lines[0])
    except json.JSONDecodeError as error:
        raise WanderFormatError(f"invalid original JSONL header: {error}") from error
    if not isinstance(header, dict):
        raise WanderFormatError("original JSONL header must be a JSON object")
    if (header.get("schema") != SCHEMA or header.get("version") != 1 or
            header.get("entry_pc") != ENTRY_PC or header.get("exit_pc") != EXIT_PC):
        raise WanderFormatError("original JSONL header/schema/PCs do not match wander trace v1")
    previous_frame = -1
    for line_number, line in enumerate(lines[1:], 2):
        try:
            pair = json.loads(line)
        except json.JSONDecodeError as error:
            raise WanderFormatError(f"invalid original JSONL line {line_number}: {error}") from error
        if not isinstance(pair, dict) or pair.get("pair") != line_number - 1:
            raise WanderFormatError(f"original pair numbering/order is invalid at line {line_number}")
        entry, exit_ = pair.get("entry"), pair.get("exit")
        if not isinstance(entry, dict) or not isinstance(exit_, dict):
            raise WanderFormatError(f"original pair {pair['pair']} lacks entry/exit objects")
        if (entry.get("phase") != "pre" or exit_.get("phase") != "post" or
                entry.get("pc") != ENTRY_PC or exit_.get("pc") != EXIT_PC):
            raise WanderFormatError(f"original pair {pair['pair']} has invalid phase or PC")
        frame = _u32(entry.get("frame"), f"pair {pair['pair']} entry.frame")
        exit_frame = _u32(exit_.get("frame"), f"pair {pair['pair']} exit.frame")
        if frame != exit_frame or not 0 <= frame < requested_ticks:
            raise WanderFormatError(f"original pair {pair['pair']} frame is inconsistent or outside requested interval")
        if frame <= previous_frame:
            raise WanderFormatError("original pairs are duplicate or out of frame order")
        previous_frame = frame
        for phase, snapshot in ((0, entry), (1, exit_)):
            if _u32(snapshot.get("field_npc_count"), "field_npc_count") != 3:
                raise WanderFormatError(f"original pair {pair['pair']} is not the three-NPC target roster")
            if _u32(snapshot.get("alias13_instance"), "alias13_instance") != _u32(snapshot.get("job_40"), "job_40"):
                raise WanderFormatError(f"original pair {pair['pair']} alias-13/job identity mismatch")
            if _u32(snapshot.get("job_34"), "job_34") != 1 or _u32(snapshot.get("job_38"), "job_38") != 1 or _u32(snapshot.get("job_58"), "job_58") != 1:
                raise WanderFormatError(f"original pair {pair['pair']} is not active in authored/current state 1")
            counter = _u32(snapshot.get("job_3c"), "job_3c")
            rng = _u32(snapshot.get("rng_state"), "rng_state")
            velocity = snapshot.get("job_20")
            if not isinstance(velocity, list) or len(velocity) != 4:
                raise WanderFormatError(f"original pair {pair['pair']} job_20 must contain four words")
            velocity = [_u32(word, "job_20 word") for word in velocity]
            channel_count = _u32(snapshot.get("channel_count"), "channel_count")
            channels = snapshot.get("channels_u32")
            if not 1 <= channel_count <= MAX_CHANNELS or not isinstance(channels, list) or len(channels) != channel_count * 4:
                raise WanderFormatError(f"original pair {pair['pair']} channel extent does not match channel_count")
            channels = [_u32(word, "channels_u32 word") for word in channels]
            snapshots.append({
                "frame": frame, "phase": phase, "pair": pair["pair"],
                "job": _u32(snapshot.get("job"), "job"),
                "actor_instance": _u32(snapshot.get("job_40"), "job_40"),
                "actor_object": _u32(snapshot.get("actor_object"), "actor_object"),
                "first_channel": _u32(snapshot.get("first_channel"), "first_channel"),
                "channel_group": _u32(snapshot.get("channel_group"), "channel_group"),
                "counter": counter, "rng": rng, "velocity": velocity,
                "channel_count": channel_count, "channels": channels,
            })
        pre, post = snapshots[-2:]
        stable = ("job", "actor_instance", "actor_object", "first_channel", "channel_group", "channel_count")
        changed = [key for key in stable if pre[key] != post[key]]
        if changed:
            raise WanderFormatError(f"original pair {pair['pair']} changed job/actor/channel provenance: {', '.join(changed)}")
    if not snapshots:
        raise WanderFormatError("original JSONL contains no callback pairs")
    return header, snapshots, len(snapshots) // 2

def load_native(path):
    try:
        data = path.read_bytes()
    except OSError as error:
        raise WanderFormatError(f"cannot read native wander binary: {error}") from error
    if not data or len(data) < NATIVE_HEADER_WORDS * 4:
        raise WanderFormatError("native wander binary is empty or shorter than its 11-word header")
    records = []
    offset = 0
    previous_key = None
    phases_by_tick = defaultdict(set)
    while offset < len(data):
        if len(data) - offset < NATIVE_HEADER_WORDS * 4:
            raise WanderFormatError(f"truncated native header at byte {offset}")
        tick, scene, alias_raw, phase, counter, rng, count, *velocity = struct.unpack_from("<11I", data, offset)
        if not 1 <= count <= MAX_CHANNELS:
            raise WanderFormatError(f"native channel_count out of bounds at byte {offset}")
        record_size = (NATIVE_HEADER_WORDS + count * 4) * 4
        if record_size > len(data) - offset:
            raise WanderFormatError(f"truncated native channel payload at byte {offset}")
        alias = struct.unpack("<i", struct.pack("<I", alias_raw))[0]
        if scene != TARGET_SCENE or alias != TARGET_ALIAS:
            raise WanderFormatError(f"native record at byte {offset} has scene/alias {scene}/{alias}, expected 68/-13")
        if phase not in (0, 1):
            raise WanderFormatError(f"native record at byte {offset} has invalid phase {phase}")
        key = (tick, phase)
        if phase in phases_by_tick[tick]:
            raise WanderFormatError(f"duplicate native callback at tick {tick}, phase {phase}")
        if previous_key is not None and key <= previous_key:
            raise WanderFormatError("native records are duplicate or out of tick/phase order")
        previous_key = key
        phases_by_tick[tick].add(phase)
        channel_words = struct.unpack_from(f"<{count * 4}I", data, offset + NATIVE_HEADER_WORDS * 4)
        records.append({
            "tick": tick, "scene": scene, "alias": alias, "phase": phase,
            "counter": counter, "rng": rng, "channel_count": count,
            "velocity": velocity, "channels": list(channel_words),
        })
        offset += record_size
    incomplete = {tick: sorted({0, 1} - phases) for tick, phases in phases_by_tick.items() if phases != {0, 1}}
    return records, incomplete

def compare(original_snapshots, native_records, first_native_tick, requested_ticks):
    expected = {}
    for snap in original_snapshots:
        tick = first_native_tick + snap["frame"]
        key = (tick, snap["phase"])
        if key in expected:
            raise WanderFormatError(f"fixed original frame mapping produces duplicate tick/phase {key}")
        expected[key] = snap
    first_tick, last_tick = first_native_tick, first_native_tick + requested_ticks - 1
    mapped_native = {(r["tick"], r["phase"]): r for r in native_records
                     if first_tick <= r["tick"] <= last_tick}
    missing = sorted(set(expected) - set(mapped_native))
    native_only = sorted(set(mapped_native) - set(expected))
    all_ticks = set(tick for tick, _ in expected) | set(tick for tick, _ in mapped_native)
    activation_mismatches = []
    for tick in sorted(all_ticks):
        original_active = any(key[0] == tick for key in expected)
        native_active = any(key[0] == tick for key in mapped_native)
        if original_active != native_active:
            activation_mismatches.append({"tick": tick, "original_active": original_active,
                                          "native_active": native_active})
    activation_ranges = []
    for item in activation_mismatches:
        if activation_ranges and activation_ranges[-1]["last_tick"] + 1 == item["tick"] and \
                activation_ranges[-1]["original_active"] == item["original_active"] and \
                activation_ranges[-1]["native_active"] == item["native_active"]:
            activation_ranges[-1]["last_tick"] = item["tick"]
        else:
            activation_ranges.append({"first_tick": item["tick"], "last_tick": item["tick"],
                                      "original_active": item["original_active"],
                                      "native_active": item["native_active"]})
    categories = {name: {"snapshots_compared": 0, "snapshots_differing": 0,
                         "raw_words_compared": 0, "raw_words_differing": 0, "first_difference": None}
                  for name in ("counter", "rng", "velocity", "channel_count", "channels")}

    def record_word_difference(category, key, index, original_word, native_word):
        item = categories[category]
        item["raw_words_compared"] += 1
        if original_word != native_word:
            item["raw_words_differing"] += 1
            if item["first_difference"] is None:
                item["first_difference"] = {
                    "tick": key[0], "phase": "pre" if key[1] == 0 else "post",
                    "original_frame": key[0] - first_native_tick,
                    "category": category, "index": index,
                    "original_u32": f"0x{original_word:08x}",
                    "native_u32": f"0x{native_word:08x}",
                }
            item["_different_keys"].add(key)

    for name in categories:
        categories[name]["_different_keys"] = set()
    for key in sorted(set(expected) & set(mapped_native)):
        original, native = expected[key], mapped_native[key]
        for category in ("counter", "rng"):
            categories[category]["snapshots_compared"] += 1
            record_word_difference(category, key, 0, original[category], native[category])
        for category in ("velocity",):
            categories[category]["snapshots_compared"] += 1
            for index, (left, right) in enumerate(zip(original[category], native[category])):
                record_word_difference(category, key, index, left, right)
        categories["channel_count"]["snapshots_compared"] += 1
        record_word_difference("channel_count", key, 0, original["channel_count"], native["channel_count"])
        categories["channels"]["snapshots_compared"] += 1
        left, right = original["channels"], native["channels"]
        for index, (left_word, right_word) in enumerate(zip(left, right)):
            record_word_difference("channels", key, index, left_word, right_word)
        if len(left) != len(right):
            item = categories["channels"]
            extent_delta = abs(len(left) - len(right))
            item["raw_words_compared"] += extent_delta
            item["raw_words_differing"] += extent_delta
            item["_different_keys"].add(key)
            if item["first_difference"] is None:
                item["first_difference"] = {
                    "tick": key[0], "phase": "pre" if key[1] == 0 else "post",
                    "original_frame": key[0] - first_native_tick,
                    "category": "channels", "index": min(len(left), len(right)),
                    "original_u32": None if len(left) <= len(right) else f"0x{left[len(right)]:08x}",
                    "native_u32": None if len(right) <= len(left) else f"0x{right[len(left)]:08x}",
                }
    for item in categories.values():
        item["snapshots_differing"] = len(item.pop("_different_keys"))
    native_outside = [r for r in native_records if not first_tick <= r["tick"] <= last_tick]
    return {
        "fixed_mapping": {"rule": "native_tick = first_native_tick + original.frame",
                          "first_native_tick": first_native_tick,
                          "original_frame_min_in_requested_interval": 0,
                          "original_frame_max_in_requested_interval": requested_ticks - 1,
                          "mapped_native_tick_min": first_tick, "mapped_native_tick_max": last_tick},
        "original_callback_snapshots": len(original_snapshots),
        "native_records_total": len(native_records),
        "native_records_in_mapped_interval": len(mapped_native),
        "matched_callback_snapshots": len(set(expected) & set(mapped_native)),
        "missing_native_observations": len(missing),
        "missing_native_examples": [{"tick": t, "phase": "pre" if p == 0 else "post"} for t, p in missing[:16]],
        "native_only_callbacks_in_mapped_interval": len(native_only),
        "native_only_examples": [{"tick": t, "phase": "pre" if p == 0 else "post"} for t, p in native_only[:16]],
        "native_records_outside_mapped_interval": len(native_outside),
        "native_outside_examples": [{"tick": r["tick"], "phase": "pre" if r["phase"] == 0 else "post"}
                                    for r in native_outside[:16]],
        "activation_mismatch_tick_count": len(activation_mismatches),
        "activation_mismatch_ranges": activation_ranges,
        "activation_mismatch_examples": activation_mismatches[:16],
        "categories": categories,
        "result": "different" if missing or native_only or any(v["raw_words_differing"] for v in categories.values()) else "exact",
    }

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, required=True, help='Original --wander JSONL trace')
    parser.add_argument('--native', type=Path, required=True, help='Native variable-record .png.wander.bin')
    parser.add_argument('--first-native-tick', type=int, required=True)
    parser.add_argument('--requested-ticks', type=int, required=True)
    parser.add_argument('--output', type=Path, help='Fresh JSON report path; default prints to stdout')
    args = parser.parse_args()
    if args.first_native_tick < 0 or args.requested_ticks < 1:
        parser.error('first-native-tick must be nonnegative and requested-ticks positive')
    try:
        header, original, pair_count = load_original(args.original, args.requested_ticks)
        native, incomplete = load_native(args.native)
        report = compare(original, native, args.first_native_tick, args.requested_ticks)
    except WanderFormatError as error:
        parser.error(str(error))
    report.update({
        "scope": "Exact fixed-anchor comparison of scene-68 alias-13 wander state; no coordinate tolerance or realignment",
        "original_path": str(args.original), "original_sha256": _hash(args.original),
        "native_path": str(args.native), "native_sha256": _hash(args.native),
        "original_jsonl_header": header, "original_pairs": pair_count,
        "native_incomplete_tick_pairs": len(incomplete),
        "native_incomplete_ticks": [{"tick": tick, "missing_phases": phases} for tick, phases in sorted(incomplete.items())[:16]],
    })
    text = json.dumps(report, indent=2, sort_keys=True) + '\n'
    if args.output:
        if args.output.exists() or args.output.is_symlink():
            parser.error(f'refusing existing output: {args.output}')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    else:
        print(text, end='')

if __name__ == '__main__':
    main()
