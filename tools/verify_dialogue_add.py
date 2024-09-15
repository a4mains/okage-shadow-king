#!/usr/bin/env python3
"""Prepare and compare bounded original/native ADD dialogue metadata probes.

The fixture builder is deterministic and never runs the original oracle. The
oracle output preserves a full 4096-byte event-variable snapshot per case.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from typing import Optional

ROW_BYTES = 160
COMMAND_BYTES = 128
VARIABLE_BYTES = 0x1000
ORIGINAL_OUTPUT_BYTES = 4 + VARIABLE_BYTES + 13 * 4
NATIVE_OUTPUT_BYTES = 4 + VARIABLE_BYTES + 10 * 4
NATIVE_FIELD_PROJECTION = (2, 3, 4, 6, 7, 8, 9, 10, 11, 12)
ORIGINAL_FIELD_NAMES = (
    "message.e48", "message.e8c", "message.e4c", "message.ed4", "message.ed8",
    "message.edc", "message.170", "message.1f0", "object.1e70", "object.1900",
    "object.cd0", "object.cd4", "object.cd8",
)
MAX_CASES = 64
CASES = (
    (0, 0, 3, 101, 0x7f, "ADD(101/3/1)"),
    (1, 0, 4, 101, 0x7fffffff, "ADD(101/4/1)"),
    (2, 0, 0, 101, 0, "ADD(101/0/1)"),
    (3, 0, 1, 101, 0, "ADD(101/1/1)"),
    (4, 0, 8, 39, 0x7fff, "ADD(39/8/1)"),
    (5, 0, 14, 39, 0x8000, "ADD(39/14/-1)"),
    (6, 0, 3, 101, 0x7f, "WINTYPE(1)ADD(101/3/1)"),
    (7, 0, 3, 101, 0x7f, "SET(102/4/9)ADD(101/3/1)"),
    (8, 0, 3, 101, 0x7f, "ADD(101/3/1)SET(102/4/9)"),
    (9, 0, 3, 101, 0x7f, "WINTYPE(1)SET(102/4/9)ADD(101/3/1)"),
    (10, 0, 3, 101, 0x7f, "SET(102/4/9)WINTYPE(1)ADD(101/3/1)"),
)
TOKEN = re.compile(r"(ADD|SET)\((-?\d+)/(-?\d+)/(-?\d+)\)|WINTYPE\((-?\d+)\)")

def fixture() -> bytes:
    output = bytearray()
    for case, choice, bank, index, seed, command in CASES:
        encoded = command.encode("ascii")
        if len(encoded) >= COMMAND_BYTES:
            raise ValueError(f"case {case}: command too long")
        output += struct.pack("<8I", case, choice, bank, index, seed & 0xffffffff, 0, 0, 0)
        output += encoded + bytes(COMMAND_BYTES - len(encoded))
    return bytes(output)

def valid_selector(bank: int, index: int) -> bool:
    if bank in (0, 1):
        return 0 <= index < 0x800
    if bank in (2, 3):
        return 0 <= index < 0x200
    if bank == 4:
        return 0 <= index < (VARIABLE_BYTES - 0x510) // 4
    if 8 <= bank <= 15:
        return 0 <= index < 40
    return False

def parse_cases(data: bytes) -> list[tuple[int, int, int, int, int, str]]:
    if not data or len(data) % ROW_BYTES or len(data) > MAX_CASES * ROW_BYTES:
        raise ValueError("Dialogue ADD input extent")
    result = []
    for number in range(len(data) // ROW_BYTES):
        at = number * ROW_BYTES
        words = struct.unpack_from("<8I", data, at)
        case, choice, bank, index, seed = words[:5]
        if words[5:] != (0, 0, 0) or choice > 2 or not valid_selector(bank, index):
            raise ValueError(f"Dialogue ADD input row {number}: invalid selectors/reserved words")
        command_field = data[at + 32:at + ROW_BYTES]
        command, separator, padding = command_field.partition(b"\0")
        if not separator or not command or any(padding):
            raise ValueError(f"Dialogue ADD input row {number}: command termination/padding")
        try:
            text = command.decode("ascii")
        except UnicodeDecodeError as exc:
            raise ValueError(f"Dialogue ADD input row {number}: command is not ASCII") from exc
        result.append((case, choice, bank, index, seed, text))
    return result

def apply_store(storage: bytearray, op: str, index: int, bank: int, value: int) -> None:
    if bank in (0, 1):
        offset = 0x10 + bank * 0x100 + index // 8
        mask = 0x80 >> (index & 7)
        if op == "SET":
            storage[offset] = (storage[offset] | mask) if value else (storage[offset] & ~mask)
    elif bank in (2, 3):
        offset = (0x210 if bank == 2 else 0x410) + index
        if op == "SET":
            storage[offset] = value & 0xff
        else:
            storage[offset] = (storage[offset] + (value & 0xff)) & 0xff
    elif bank == 4:
        offset = 0x510 + index * 4
        previous = int.from_bytes(storage[offset:offset + 4], "little")
        result = value & 0xffffffff if op == "SET" else (previous + value) & 0xffffffff
        storage[offset:offset + 4] = result.to_bytes(4, "little")
    elif 8 <= bank <= 15:
        offset = 0x5c4 + (bank & 7) * 0x50 + index * 2
        previous = int.from_bytes(storage[offset:offset + 2], "little")
        result = value & 0xffff if op == "SET" else (previous + value) & 0xffff
        storage[offset:offset + 2] = result.to_bytes(2, "little")
    else:
        raise ValueError(f"Unsupported fixture bank {bank}")

def expected(case: tuple[int, int, int, int, int, str]) -> tuple[bytes, int]:
    case_id, _, seed_bank, seed_index, seed, command = case
    storage = bytearray([0xa5] * VARIABLE_BYTES)
    if seed_bank in (0, 1):
        apply_store(storage, "SET", seed_index, seed_bank, seed & 1)
    elif seed_bank in (2, 3):
        offset = (0x210 if seed_bank == 2 else 0x410) + seed_index
        storage[offset] = seed & 0xff
    elif seed_bank == 4:
        storage[0x510 + seed_index * 4:0x514 + seed_index * 4] = (seed & 0xffffffff).to_bytes(4, "little")
    elif 8 <= seed_bank <= 15:
        offset = 0x5c4 + (seed_bank & 7) * 0x50 + seed_index * 2
        storage[offset:offset + 2] = (seed & 0xffff).to_bytes(2, "little")
    else:
        raise ValueError(f"case {case_id}: unsupported seed bank")
    last_return = 0xffffffff
    cursor = 0
    for match in TOKEN.finditer(command):
        if match.start() != cursor:
            raise ValueError(f"case {case_id}: unsupported command syntax at {cursor}")
        cursor = match.end()
        if match[1]:
            op, index, bank, value = match[1], int(match[2]), int(match[3]), int(match[4])
            if not valid_selector(bank, index):
                raise ValueError(f"case {case_id}: command selector outside probe scope")
            apply_store(storage, op, index, bank, value)
            if op == "SET":
                last_return = 12
        else:
            last_return = 7
    if cursor != len(command):
        raise ValueError(f"case {case_id}: unsupported command suffix at {cursor}")
    return bytes(storage), last_return

def verify(inputs: bytes, original: bytes, handler_returns: bytes,
           native: Optional[bytes] = None) -> dict:
    cases = parse_cases(inputs)
    original_required = len(cases) * ORIGINAL_OUTPUT_BYTES
    native_required = len(cases) * NATIVE_OUTPUT_BYTES
    if len(original) != original_required:
        raise ValueError(f"Original output extent: expected {original_required}, got {len(original)}")
    if len(handler_returns) != len(cases) * 8:
        raise ValueError(f"Original handler-return sidecar extent: expected {len(cases) * 8}, got {len(handler_returns)}")
    if native is not None and len(native) != native_required:
        raise ValueError(f"Native output extent: expected {native_required}, got {len(native)}")
    cases_report = []
    for number, case in enumerate(cases):
        at = number * ORIGINAL_OUTPUT_BYTES
        (case_id,) = struct.unpack_from("<I", original, at)
        if case_id != case[0]:
            raise ValueError(f"Original row {number}: case id {case_id} != {case[0]}")
        return_case, handler_return = struct.unpack_from("<2I", handler_returns, number * 8)
        if return_case != case[0]:
            raise ValueError(f"Original handler-return row {number}: case id {return_case} != {case[0]}")
        expected_variables, expected_return = expected(case)
        original_scalars = struct.unpack_from("<13I", original, at + 4 + VARIABLE_BYTES)
        if [original_scalars[i] for i in (0, 1, 5)] != [1, 1, 1]:
            raise ValueError(f"Original case {case_id}: wrapper/other metadata fields 0/1/5 changed")
        variables_at = at + 4
        actual_variables = original[variables_at:variables_at + VARIABLE_BYTES]
        if actual_variables != expected_variables:
            diff = next(i for i, (a, b) in enumerate(zip(actual_variables, expected_variables)) if a != b)
            raise ValueError(f"Original case {case_id}: variable bank differs at +0x{diff:x}")
        if handler_return != expected_return:
            raise ValueError(f"Original case {case_id}: handler return {handler_return:#x}, expected {expected_return:#x}")
        if native is not None:
            native_at = number * NATIVE_OUTPUT_BYTES
            native_case = struct.unpack_from("<I", native, native_at)[0]
            if native_case != case_id:
                raise ValueError(f"Case {case_id}: native case id {native_case} != {case_id}")
            native_variables_at = native_at + 4
            native_variables = native[native_variables_at:native_variables_at + VARIABLE_BYTES]
            if actual_variables != native_variables:
                diff = next(i for i, (a, b) in enumerate(zip(actual_variables, native_variables)) if a != b)
                raise ValueError(f"Case {case_id}: original/native first differs at variables+0x{diff:x}")
            native_scalars_at = native_variables_at + VARIABLE_BYTES
            actual_native_scalars = struct.unpack_from("<10I", native, native_scalars_at)
            expected_native_scalars = tuple(original_scalars[i] for i in NATIVE_FIELD_PROJECTION)
            if actual_native_scalars != expected_native_scalars:
                scalar = next(i for i, (a, b) in enumerate(zip(actual_native_scalars, expected_native_scalars)) if a != b)
                original_index = NATIVE_FIELD_PROJECTION[scalar]
                raise ValueError(f"Case {case_id}: original/native controller scalar {scalar} "
                                 f"(original field {original_index}) differs: "
                                 f"{actual_native_scalars[scalar]:#x} != {expected_native_scalars[scalar]:#x}")
        cases_report.append({"case": case_id, "command": case[5], "handler_return": handler_return,
                             "seed_bank": case[2], "seed_index": case[3], "seed_value": case[4],
                             "original_controller_fields": list(original_scalars),
                             "original_fields_outside_native_projection": {
                                 str(i): original_scalars[i] for i in (0, 1, 5)}})
    return {"schema": "okage-original-dialogue-add-v1", "cases": cases_report,
            "original_output_bytes_per_case": ORIGINAL_OUTPUT_BYTES,
            "native_output_bytes_per_case": NATIVE_OUTPUT_BYTES,
            "original_field_names": list(ORIGINAL_FIELD_NAMES),
            "native_field_projection": list(NATIVE_FIELD_PROJECTION),
            "handler_return_sidecar_bytes": len(handler_returns),
            "sha256": {"input": hashlib.sha256(inputs).hexdigest(),
                       "original": hashlib.sha256(original).hexdigest(),
                       "handler_returns": hashlib.sha256(handler_returns).hexdigest(),
                       **({"native": hashlib.sha256(native).hexdigest()} if native is not None else {})},
            "exact_native_match": True if native is not None else None}

def write_exact(path: Path, data: bytes) -> None:
    if path.is_symlink():
        raise ValueError(f"Refusing symlink output {path}")
    if path.exists():
        if path.read_bytes() != data:
            raise ValueError(f"Refusing different existing output {path}")
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser("prepare")
    prepare.add_argument("output", type=Path)
    compare = commands.add_parser("verify")
    compare.add_argument("input", type=Path)
    compare.add_argument("original", type=Path)
    compare.add_argument("--handler-returns", type=Path)
    compare.add_argument("native", type=Path, nargs="?")
    compare.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.command == "prepare":
        data = fixture()
        write_exact(args.output, data)
        print(f"{len(parse_cases(data))} cases; sha256 {hashlib.sha256(data).hexdigest()}")
    else:
        handler_path = args.handler_returns or Path(str(args.original) + ".handler-returns.bin")
        report = verify(args.input.read_bytes(), args.original.read_bytes(), handler_path.read_bytes(),
                        args.native.read_bytes() if args.native else None)
        output = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode()
        if args.report:
            write_exact(args.report, output)
        print(output.decode(), end="")

if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc
