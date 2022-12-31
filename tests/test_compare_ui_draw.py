import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import compare_ui_draw as verifier

def fixtures():
    header = {"schema": verifier.REFERENCE_SCHEMA,
              "source_elf_sha256": verifier.SOURCE_ELF_SHA256,
              "scope": "CPU submission calls inside the first source-gated 1a8150 draw tail-call; no deferred GS vertices or raster output"}
    rows = [header,
        {"event": "draw_submission_entry", "frame": 127, "ra": 1234, "sp": 9876,
         "shared_dispatch_return": 1234, "root": 4000, "manager": 5000,
         "windows": [{"slot": 0, "flags_4f8": 0}, {"slot": 1, "flags_4f8": 64}]},
    ]
    def call(cid, callee, payload=None):
        row = {"event": "call_entry", "call_id": cid, "callee": f"0x{callee:08x}",
               "frame": 127, "pc": callee, "ra": 2000 + cid * 8, "sp": 9000 - cid * 16}
        if payload:
            row.update(payload)
        rows.append(row)
        return row
    def ret(entry):
        rows.append({"event": "call_return", "call_id": entry["call_id"], "callee": entry["callee"],
                     "frame": 127, "pc": 0xdead, "ra": entry["ra"], "sp": entry["sp"]})
    call_row = call(1, 0x1ad850, {"outer_projection_vectors": {"raw_hex": "0000803f" * 32}}); ret(call_row)
    call_row = call(2, 0x1ad3c8, {"vector_upload": {"raw_hex": "0000803f" * 16}}); ret(call_row)
    call_row = call(3, 0x1aeb10, {"a1": 1, "callsite": "0x001a859c"}); ret(call_row)
    call_row = call(4, 0x1ae880, {"callsite": "0x001a85b0", "f12_f15_bits": [1124073472, 1124073472, 1124073472, 0]}); ret(call_row)
    call_row = call(5, 0x1aeb10, {"a1": 2, "callsite": "0x001a8bb4"}); ret(call_row)
    call_row = call(6, 0x1ae880, {"callsite": "0x001a8bc8", "f12_f15_bits": [1124073472, 1124073472, 1124073472, 0]}); ret(call_row)
    call_row = call(7, 0x1ae880, {"callsite": "0x001a8bdc", "f12_f15_bits": [1124073472, 1124073472, 1124073472, 0]}); ret(call_row)
    call_row = call(8, 0x1aeb48, {"selected_element_color_row": {"raw_hex": "0000803f" * 4}}); ret(call_row)
    call_row = call(9, 0x1aedd8, {"gs_register": 7, "gs_value_a1_u64": "0x0000000000000009"}); ret(call_row)
    call_row = call(10, 0x1ad3c0, {"packet_owner": {"window_slot": 0, "family": "sprite",
        "draw_type": "sprite_element", "group": 2, "side": 1, "element": 3, "pass_index": 0}})
    rows.append({"event": "thunk_alias_entry", "call_id": 10, "aliases": "0x001ad3c0",
                 "frame": 127, "pc": 0x1c9368, "ra": call_row["ra"], "sp": call_row["sp"]})
    ret(call_row)
    rows.extend([{ "event": "draw_dispatch_completion", "frame": 127, "ra": 1234,
                   "shared_return": 1234, "sp": 9876, "root": 4000, "manager": 5000, "helper_calls": 10 },
                 {"event": "finish", "draw_completed": True, "helper_calls": 10}])
    reference = {"rows": rows}
    commands = [
        {"kind": "projection", "float_bits": ["3f800000"] * 32},
        {"kind": "matrix", "float_bits": ["3f800000"] * 16},
        {"kind": "vectors", "vector_kind": "auxiliary", "values_f32_bits": [["43000000", "43000000", "43000000", "00000000"]]},
        {"kind": "vectors", "vector_kind": "auxiliary", "values_f32_bits": [["43000000", "43000000", "43000000", "00000000"]] * 2},
        {"kind": "vectors", "vector_kind": "colors", "values_f32_bits": [["3f800000"] * 4]},
        {"kind": "gs_register", "register": 7, "value_u64": "0000000000000009"},
        {"kind": "replay", "owner": {"family": "sprite_element", "window": 0, "group": 2,
          "side": 1, "element": 3, "history": 0, "pass": 0},
         "packet": {"operations": [], "elements": [None] * 16}},
    ]
    for i, command in enumerate(commands):
        command["index"] = i
    native = {"schema": verifier.NATIVE_SCHEMA,
        "scope": "ordered native UiDrawList snapshot; no GPU submission, GS execution, vertices, or raster equivalence",
        "frame": 104445, "input_anchor": 104376, "reference_vblank": 127,
        "execution": "actual-session-first-resource-after-callbacks", "command_count": len(commands),
        "packet_operation_count": 0,
        "commands": commands}
    return reference, native

class CompareUiDrawTests(unittest.TestCase):
    def test_exact_common_channels_and_alias_is_not_double_counted(self):
        reference, native = fixtures()
        with tempfile.TemporaryDirectory() as directory:
            ref = Path(directory) / "source.jsonl"
            ref.write_text("\n".join(json.dumps(row) for row in reference["rows"]) + "\n")
            nat = Path(directory) / "native.json"
            nat.write_text(json.dumps(native))
            source = verifier.load_reference(ref)
            actual = verifier.load_native(nat)
            self.assertEqual(verifier.compare(source, actual, 104376, 127), [])
            with self.assertRaises(verifier.Invalid):
                verifier.compare(source, actual, 104376, 126)
            self.assertEqual(source["aliases"], 1)
            self.assertEqual(source["replays"][0]["element"], 3)

    def test_source_and_native_data_corruption_is_reported(self):
        reference, native = fixtures()
        native["commands"][-1]["owner"]["group"] = 9
        with tempfile.TemporaryDirectory() as directory:
            ref = Path(directory) / "source.jsonl"
            ref.write_text("\n".join(json.dumps(row) for row in reference["rows"]) + "\n")
            nat = Path(directory) / "native.json"
            nat.write_text(json.dumps(native))
            diff = verifier.compare(verifier.load_reference(ref), verifier.load_native(nat), 104376, 127)
            replay_diff = next(item for item in diff if item["field"] == "replays")
            self.assertEqual(replay_diff["first_index"], 0)

    def test_source_identity_and_totals_are_required(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.jsonl"
            for change in ("elf", "completion_count", "finish_count"):
                reference, _ = fixtures()
                rows = reference["rows"]
                if change == "elf":
                    rows[0]["source_elf_sha256"] = "0" * 64
                else:
                    rows[-2 if change == "completion_count" else -1]["helper_calls"] = 9
                path.write_text("\n".join(json.dumps(row) for row in rows) + "\n")
                with self.subTest(change=change), self.assertRaises(verifier.Invalid):
                    verifier.load_reference(path)

    def test_cross_kind_reorder_fails_unified_stream(self):
        reference, native = fixtures()
        native["commands"][1], native["commands"][4] = native["commands"][4], native["commands"][1]
        for index, command in enumerate(native["commands"]):
            command["index"] = index
        with tempfile.TemporaryDirectory() as directory:
            ref, nat = Path(directory) / "source.jsonl", Path(directory) / "native.json"
            ref.write_text("\n".join(json.dumps(row) for row in reference["rows"]) + "\n")
            nat.write_text(json.dumps(native))
            original = verifier.load_reference(ref)
            actual = verifier.load_native(nat)
            self.assertEqual(original["matrices"], actual["matrices"])
            self.assertEqual(original["colors"], actual["colors"])
            self.assertNotEqual(original["stream"], actual["stream"])
            differences = verifier.compare(original, actual, 104376, 127)
            self.assertIn("stream", [item["field"] for item in differences])

    def test_rejects_unbalanced_alias_and_misaligned_execution(self):
        reference, native = fixtures()
        reference["rows"][-3]["call_id"] = 99
        with tempfile.TemporaryDirectory() as directory:
            ref = Path(directory) / "source.jsonl"
            ref.write_text("\n".join(json.dumps(row) for row in reference["rows"]) + "\n")
            with self.assertRaises(verifier.Invalid):
                verifier.load_reference(ref)
            reference, native = fixtures()
            native["execution"] = "constructor-smoke"
            nat = Path(directory) / "native.json"
            nat.write_text(json.dumps(native))
            with self.assertRaises(verifier.Invalid):
                verifier.load_native(nat)

    def test_duplicate_json_keys_and_malformed_packet_are_rejected(self):
        _, native = fixtures()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "native.json"
            path.write_text('{"schema":"wrong","schema":"wrong"}')
            with self.assertRaises(verifier.Invalid):
                verifier.load_native(path)
            native["commands"][-1]["packet"]["elements"] = [None]
            path.write_text(json.dumps(native))
            with self.assertRaises(verifier.Invalid):
                verifier.load_native(path)

    def test_cli_exit_codes_and_output_clobber_guard(self):
        reference, native = fixtures()
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            ref, nat, out = directory / "ref.jsonl", directory / "native.json", directory / "out.json"
            ref.write_text("\n".join(json.dumps(row) for row in reference["rows"]) + "\n")
            nat.write_text(json.dumps(native))
            command = [sys.executable, str(ROOT / "tools/compare_ui_draw.py"), "--reference", str(ref),
                       "--native", str(nat), "--input-anchor", "104376", "--original-vblank", "127", "--output", str(out)]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)
            native["commands"][0]["float_bits"][0] = "00000001"
            nat.write_text(json.dumps(native))
            out2 = directory / "different.json"
            command[-1] = str(out2)
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)

if __name__ == "__main__":
    unittest.main()
