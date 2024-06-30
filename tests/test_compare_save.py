import importlib.util
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).parents[1]
TOOL = ROOT / "tools/compare_save.py"
SPEC = importlib.util.spec_from_file_location("compare_save", TOOL)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)

def source_event(frame, edge, *, state=10, selected=0, completed=0, dead=False, cursor=0, alpha=(30, 30, 30)):
    pc = MODULE.INPUT_HANDLER_PC if edge == "input_handler_entry" else MODULE.SECOND_CALLBACK_PC
    globals_obj = {addr: "0x00000000" for _, addr, _array_index, _ in MODULE.GLOBAL_FIELDS}
    globals_obj["24ee10"] = [f"0x{value:08x}" for value in alpha]
    globals_obj.update({
        "1f89a8": f"0x{state:08x}",
        "1f89ac": f"0x{selected:08x}",
        "24ecc0": f"0x{completed:08x}",
        "24ecc4": "0x3f800000",
        "24ecd8": "0x00000004",
    })
    flags = 0x80000000 if dead else 0
    obs = {
        "owner_kind": "field_mode2_save",
        "edge": edge,
        "parent_mode40": "0x00000002",
        "window_callback_9c": "0x0011c2c8",
        "window_type_a0": "0x00000008",
        "descriptor2234f0": ["0x0011f668", "0x0011fec8", "0x00120ad0", "0x00120b48"],
        "save_selector_1f89a0": "0x00000000",
        "source_globals": globals_obj,
        "manager_flags1c": f"0x{flags:08x}",
        "manager_window_count_e40": "0x00000001",
        "manager_layer_index_e48": "0x00000000",
        "manager_dispatch_state_e4c": "0x00000000",
        "manager_layer_state_cursor_e8c": f"0x{cursor:08x}",
    }
    return {"frame": frame, "pc": pc, "save": obs}

def parent_event(frame, phase=0):
    return {"frame": frame, "pc": MODULE.PARENT_ENTRY_PC,
            "parent_current40": "0x00000002",
            "parent_phase20": f"0x{phase:08x}"}

def native_event(frame, *, state=10, selected=0, completed=0, dead=0, manager=1, cursor=0, alpha=(30, 30, 30)):
    row = [0] * MODULE.NATIVE_WORDS
    row[0] = frame
    row[2] = manager
    row[3] = dead
    row[4] = completed
    row[5] = state
    row[6] = selected
    row[7] = 0
    row[11] = 0x3F800000
    row[13], row[16], row[19] = alpha
    row[12] = 4
    row[22 + 0x1C // 4] = 0x80000000 if dead else 0
    row[22 + 0xE40 // 4] = 1
    row[22 + 0xE48 // 4] = 0
    row[22 + 0xE4C // 4] = 0
    row[22 + 0xE8C // 4] = cursor
    return struct.pack(f"<{MODULE.NATIVE_WORDS}I", *row)

class CompareSaveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.source = self.root / "mode.jsonl"
        self.native = self.root / "save-state.bin"
        self.output = self.root / "report.json"

    def tearDown(self):
        self.temp.cleanup()

    def write_source(self, rows, *, schema=MODULE.SOURCE_SCHEMA, elf=MODULE.SOURCE_ELF_SHA256):
        with self.source.open("w", encoding="utf-8") as stream:
            stream.write(json.dumps({"schema": schema, "elf_sha256": elf}) + "\n")
            for row in rows:
                stream.write(json.dumps(row) + "\n")

    def test_callback_edges_use_fixed_one_tick_distinction(self):
        rows = [parent_event(0),
                source_event(0, "input_handler_entry", cursor=0, alpha=(30, 30, 30)),
                source_event(0, "second_callback_entry", cursor=1, alpha=(34, 30, 30))]
        self.write_source(rows)
        self.native.write_bytes(native_event(73654, cursor=1) +
                                native_event(73655, cursor=1, alpha=(34, 30, 30)))
        source_save, source_parent = MODULE.load_source(self.source, 240)
        native = MODULE.load_native(self.native)
        report = MODULE.compare(source_save, source_parent, native, case="choice",
                                native_offset=73655, source_stop=240)
        checks = report["save_state"]["checks"]
        self.assertEqual(checks[0]["native_frame"], 73654)
        self.assertEqual(checks[1]["native_frame"], 73655)
        self.assertEqual(report["save_state"]["compared_records"], 2)
        self.assertEqual(report["save_state"]["difference_words"], 0)

    def test_reports_fixed_anchor_mismatches_and_missing_records(self):
        self.write_source([parent_event(0), source_event(0, "second_callback_entry", selected=1)])
        self.native.write_bytes(native_event(73655, selected=2))
        source_save, source_parent = MODULE.load_source(self.source, 240)
        report = MODULE.compare(source_save, source_parent, MODULE.load_native(self.native), case="choice",
                                native_offset=73655, source_stop=240)
        state = report["save_state"]
        self.assertEqual(state["first_difference"]["difference"]["field"], "selected")
        self.assertEqual(state["missing_source_count"], 1)
        self.assertEqual(state["missing_native_count"], 0)
        self.assertFalse(report["paired_state_equal"])

    def test_second_callback_entry_writes_are_retained_but_boundary_classified(self):
        self.write_source([parent_event(0), source_event(0, "second_callback_entry", alpha=(0, 0, 0))])
        self.native.write_bytes(native_event(73655, alpha=(30, 30, 30)))
        source_save, source_parent = MODULE.load_source(self.source, 240)
        report = MODULE.compare(source_save, source_parent, MODULE.load_native(self.native), case="choice",
                                native_offset=73655, source_stop=240)
        state = report["save_state"]
        self.assertEqual(state["difference_words"], 3)
        self.assertEqual(state["callback_boundary_sensitive_difference_words"], 3)
        self.assertEqual(state["comparable_difference_words"], 0)
        self.assertEqual(state["first_difference"]["difference"]["classification"],
                         "callback_boundary_sensitive")
        self.assertIsNone(state["first_comparable_difference"])
        self.assertFalse(report["paired_state_equal"])

    def test_missing_native_record_and_inactive_manager_are_distinct(self):
        rows = [parent_event(0), source_event(0, "second_callback_entry"),
                parent_event(1), source_event(1, "second_callback_entry")]
        self.write_source(rows)
        self.native.write_bytes(native_event(73655, manager=0))
        source_save, source_parent = MODULE.load_source(self.source, 240)
        report = MODULE.compare(source_save, source_parent, MODULE.load_native(self.native), case="choice",
                                native_offset=73655, source_stop=240)
        state = report["save_state"]
        self.assertEqual(state["missing_native_count"], 2)
        self.assertEqual(state["missing_native_by_reason"], {
            "native_record_absent": 1,
            "save_manager_absent": 1,
        })
        self.assertEqual(state["missing_native_record_by_position"], {
            "after_last_native_record": 1,
        })
        self.assertEqual(state["missing_native"][0]["status"], "save_manager_absent")
        self.assertEqual(state["missing_native"][1]["status"], "native_record_absent")
        self.assertEqual(report["native"]["source_active_frames_missing_native_record"], [1])
        self.assertEqual(report["native"]["source_active_frames_with_record_but_no_save_manager"], [0])

    def test_rejects_schema_owner_selector_and_extent_errors(self):
        self.write_source([parent_event(0), source_event(0, "second_callback_entry")], schema="bad")
        with self.assertRaisesRegex(MODULE.ComparisonError, "schema"):
            MODULE.load_source(self.source, 240)
        self.write_source([parent_event(0), source_event(0, "second_callback_entry")])
        self.native.write_bytes(b"x")
        with self.assertRaisesRegex(MODULE.ComparisonError, "exact 3912-byte"):
            MODULE.load_native(self.native)
        row = source_event(0, "second_callback_entry")
        row["save"]["save_selector_1f89a0"] = "0x00000001"
        self.write_source([parent_event(0), row])
        with self.assertRaisesRegex(MODULE.ComparisonError, "selector is not zero"):
            MODULE.load_source(self.source, 240)

    def test_cli_requires_known_case_anchor_and_fresh_output(self):
        self.write_source([parent_event(0)])
        self.native.write_bytes(native_event(73655))
        result = subprocess.run([sys.executable, str(TOOL), "--case", "choice",
                                 "--source", str(self.source), "--native-save-state", str(self.native),
                                 "--output", str(self.output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertTrue(self.output.exists())
        second = subprocess.run([sys.executable, str(TOOL), "--case", "choice",
                                 "--source", str(self.source), "--native-save-state", str(self.native),
                                 "--output", str(self.output)], capture_output=True, text=True)
        self.assertEqual(second.returncode, 2)
        self.assertIn("Refusing existing output", second.stderr)

    def test_open_anchor_is_fixed_and_cannot_be_fitted(self):
        source_save, source_parent = {}, {0: 0}
        native = {73895: tuple([73895] + [0] * (MODULE.NATIVE_WORDS - 1))}
        with self.assertRaisesRegex(MODULE.ComparisonError, "open requires fixed native offset 73895"):
            MODULE.compare(source_save, source_parent, native, case="open",
                           native_offset=73894, source_stop=360)

    def test_paired_write_anchor_is_fixed_full_replay_interval(self):
        source_save, source_parent = {}, {0: 0}
        native = {}
        with self.assertRaisesRegex(MODULE.ComparisonError, "paired-write requires fixed native offset 73655"):
            MODULE.compare(source_save, source_parent, native, case="paired-write",
                           native_offset=73656, source_stop=840)

if __name__ == "__main__":
    unittest.main()
