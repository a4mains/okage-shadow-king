import contextlib
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path

from tools import compare_results_live as compare

ANCHOR = 500
FIELDS = {
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

def source_row(frame, base=0):
    results = {"edge": "parent_entry", "manager_raw_pointer": "0x01dbd4a0"}
    for index, (name, key) in enumerate(FIELDS.items()):
        results[key] = f"0x{base + frame * 0x100 + index + 1:08x}"
    return {"frame": frame, "pc": compare.PARENT_ENTRY_PC, "results": results}

def native_row(frame, raw_frame, pending=0xfeed):
    values = {name: raw_frame * 0x100 + index + 1 for index, name in enumerate(compare.COMPARE_FIELDS)}
    return (frame, values["phase"], values["ui_stage"], values["complete"],
            values["counter"], pending, values["state"], values["command"],
            values["ready"], values["notice"], values["row"], values["wait"],
            values["reward_display"], values["money_display"], values["currency"],
            values["rng"])

def write_reference(path, rows, header=None):
    head = header or {"schema": compare.SCHEMA, "results_extension": compare.EXTENSION}
    path.write_text("\n".join([json.dumps(head), *(json.dumps(row) for row in rows)]) + "\n")

def write_native(path, rows):
    path.write_bytes(b"".join(struct.pack("<16I", *row) for row in rows))

class ResultsLiveComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.reference_path = self.root / "reference.jsonl"
        self.native_path = self.root / "native.bin"
        self.output_path = self.root / "report.json"
        write_reference(self.reference_path, [source_row(0), source_row(1)])
        write_native(self.native_path, [native_row(ANCHOR - 1, 0), native_row(ANCHOR, 1)])

    def tearDown(self):
        self.temp.cleanup()

    def test_exact_interval_and_preceding_native_frame_mapping(self):
        reference = compare.load_reference(self.reference_path, ANCHOR, 2)
        native = compare.load_native(self.native_path, ANCHOR, 2)
        report = compare.compare(reference, native, ANCHOR, 2)
        self.assertTrue(report["equal"])
        self.assertEqual(report["native_frame_interval"], {"first": ANCHOR - 1, "last": ANCHOR})
        self.assertEqual(report["checks"][0]["source_parent_entry_frame"], ANCHOR)
        self.assertEqual(report["checks"][0]["native_preceding_completed_frame"], ANCHOR - 1)
        self.assertEqual(report["checks"][1]["native_preceding_completed_frame"], ANCHOR)
        self.assertNotIn("pending", report["checks"][0]["native"])

    def test_pending_is_explicitly_excluded(self):
        write_native(self.native_path, [native_row(ANCHOR - 1, 0, pending=1),
                                        native_row(ANCHOR, 1, pending=0xFFFFFFFF)])
        report = compare.compare(compare.load_reference(self.reference_path, ANCHOR, 2),
                                 compare.load_native(self.native_path, ANCHOR, 2), ANCHOR, 2)
        self.assertTrue(report["equal"])
        self.assertEqual(report["difference_count"], 0)

    def test_first_difference_and_counts_are_retained(self):
        row = list(native_row(ANCHOR - 1, 0))
        row[6] ^= 1  # state
        write_native(self.native_path, [tuple(row), native_row(ANCHOR, 1)])
        report = compare.compare(compare.load_reference(self.reference_path, ANCHOR, 2),
                                 compare.load_native(self.native_path, ANCHOR, 2), ANCHOR, 2)
        self.assertFalse(report["equal"])
        self.assertEqual(report["difference_count"], 1)
        self.assertEqual(report["difference_counts_by_field"], {"state": 1})
        self.assertEqual(report["first_difference"]["field"], "state")
        self.assertEqual(report["first_difference"]["native_frame"], ANCHOR - 1)

    def test_missing_original_record_is_reported_without_shifting(self):
        write_reference(self.reference_path, [source_row(0)])
        reference = compare.load_reference(self.reference_path, ANCHOR, 2)
        report = compare.compare(reference, compare.load_native(self.native_path, ANCHOR, 2), ANCHOR, 2)
        self.assertFalse(report["complete"])
        self.assertEqual(report["missing_source_records"], [{"raw_frame": 1, "source_frame": ANCHOR + 1}])
        self.assertEqual(report["checks"][1]["native_preceding_completed_frame"], ANCHOR)

    def test_missing_native_record_is_reported(self):
        write_native(self.native_path, [native_row(ANCHOR - 1, 0)])
        report = compare.compare(compare.load_reference(self.reference_path, ANCHOR, 2),
                                 compare.load_native(self.native_path, ANCHOR, 2), ANCHOR, 2)
        self.assertFalse(report["complete"])
        self.assertEqual(report["missing_native_records"], [{"raw_frame": 1, "native_frame": ANCHOR}])

    def test_original_unknown_manager_fields_are_not_fabricated(self):
        row = source_row(0)
        row["results"]["manager_raw_pointer"] = None
        row["results"].pop("state_e4c")
        row["results"].pop("command_a8")
        write_reference(self.reference_path, [row, source_row(1)])
        report = compare.compare(compare.load_reference(self.reference_path, ANCHOR, 2),
                                 compare.load_native(self.native_path, ANCHOR, 2), ANCHOR, 2)
        self.assertFalse(report["complete"])
        self.assertEqual(report["unknown_field_count"], 2)
        self.assertEqual([x["field"] for x in report["unknown_fields"]], ["state", "command"])

    def test_wrong_native_frame_boundary_is_rejected(self):
        write_native(self.native_path, [native_row(ANCHOR, 0), native_row(ANCHOR + 1, 1)])
        with self.assertRaisesRegex(compare.ComparisonError, "outside requested interval"):
            compare.load_native(self.native_path, ANCHOR, 2)

    def test_duplicate_and_out_of_order_source_frames_are_rejected(self):
        write_reference(self.reference_path, [source_row(0), source_row(0)])
        with self.assertRaisesRegex(compare.ComparisonError, "Duplicate original"):
            compare.load_reference(self.reference_path, ANCHOR, 2)
        write_reference(self.reference_path, [source_row(1), source_row(0)])
        with self.assertRaisesRegex(compare.ComparisonError, "out of order"):
            compare.load_reference(self.reference_path, ANCHOR, 2)

    def test_duplicate_and_out_of_order_native_frames_are_rejected(self):
        write_native(self.native_path, [native_row(ANCHOR - 1, 0), native_row(ANCHOR - 1, 1)])
        with self.assertRaisesRegex(compare.ComparisonError, "duplicate or out of order"):
            compare.load_native(self.native_path, ANCHOR, 2)
        write_native(self.native_path, [native_row(ANCHOR, 0), native_row(ANCHOR - 1, 1)])
        with self.assertRaisesRegex(compare.ComparisonError, "duplicate or out of order"):
            compare.load_native(self.native_path, ANCHOR, 2)

    def test_malformed_schema_hex_and_binary_extent_are_rejected(self):
        write_reference(self.reference_path, [source_row(0), source_row(1)],
                        {"schema": "wrong", "results_extension": compare.EXTENSION})
        with self.assertRaisesRegex(compare.ComparisonError, "schema"):
            compare.load_reference(self.reference_path, ANCHOR, 2)
        write_reference(self.reference_path, [source_row(0), source_row(1)],
                        {"schema": compare.SCHEMA, "results_extension": "wrong"})
        with self.assertRaisesRegex(compare.ComparisonError, "results_extension"):
            compare.load_reference(self.reference_path, ANCHOR, 2)
        row = source_row(0)
        row["results"]["field_phase48"] = "not-hex"
        write_reference(self.reference_path, [row, source_row(1)])
        with self.assertRaisesRegex(compare.ComparisonError, "hexadecimal"):
            compare.load_reference(self.reference_path, ANCHOR, 2)
        self.native_path.write_bytes(b"short")
        with self.assertRaisesRegex(compare.ComparisonError, "expected exact"):
            compare.load_native(self.native_path, ANCHOR, 2)

    def test_cli_reports_malformed_input_and_refuses_output_clobber(self):
        args = ["--reference", str(self.reference_path), "--native", str(self.native_path),
                "--source-anchor", str(ANCHOR), "--source-frames", "2", "--output", str(self.output_path)]
        self.native_path.write_bytes(b"bad")
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            self.assertEqual(compare.main(args), 2)
        self.assertIn("compare_results_live:", stderr.getvalue())
        self.output_path.write_text("keep")
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                compare.main(args)
        self.assertEqual(raised.exception.code, 2)
        self.assertEqual(self.output_path.read_text(), "keep")

    def test_cli_writes_fresh_report_and_returns_difference_status(self):
        row = list(native_row(ANCHOR - 1, 0))
        row[8] ^= 1  # ready
        write_native(self.native_path, [tuple(row), native_row(ANCHOR, 1)])
        args = ["--reference", str(self.reference_path), "--native", str(self.native_path),
                "--source-anchor", str(ANCHOR), "--source-frames", "2", "--output", str(self.output_path)]
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(compare.main(args), 1)
        report = json.loads(self.output_path.read_text())
        self.assertEqual(report["first_difference"]["field"], "ready")
        self.assertFalse(report["equal"])

if __name__ == "__main__":
    unittest.main()
