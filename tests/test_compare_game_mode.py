import importlib.util
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).parents[1]
TOOL = ROOT / "tools/compare_game_mode.py"
SPEC = importlib.util.spec_from_file_location("compare_game_mode", TOOL)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)

def observer_row(frame, *, phase=5, current=3, arg1=11, arg2=12, request=13,
                 queued1=14, queued2=15, child=0x100100, linked=0x100100,
                 pending=(7, 8, 9)):
    return {
        "frame": frame,
        "pc": MODULE.PARENT_ENTRY_PC,
        "parent_phase20": f"0x{phase:08x}",
        "parent_current40": f"0x{current:08x}",
        "parent_arg44": f"0x{arg1:08x}",
        "parent_arg4c": f"0x{arg2:08x}",
        "parent_request3c": f"0x{request:08x}",
        "parent_queued48": f"0x{queued1:08x}",
        "parent_queued50": f"0x{queued2:08x}",
        "parent_child38": f"0x{child:08x}",
        "parent_linked_first18": f"0x{linked:08x}",
        "pending_24f740": f"0x{pending[0]:08x}",
        "pending_24f744": f"0x{pending[1]:08x}",
        "pending_24f748": f"0x{pending[2]:08x}",
    }

def expected(row):
    child = int(row["parent_child38"], 16)
    linked = int(row["parent_linked_first18"], 16)
    return (
        int(row["parent_phase20"], 16), int(row["parent_current40"], 16),
        int(row["parent_arg44"], 16), int(row["parent_arg4c"], 16),
        int(row["parent_request3c"], 16), int(row["parent_queued48"], 16),
        int(row["parent_queued50"], 16), int(child != 0), int(linked != 0 and child == 0),
        int(row["pending_24f740"], 16), int(row["pending_24f744"], 16),
        int(row["pending_24f748"], 16),
    )

def native_record(frame, point, values=None):
    values = values or (5, 3, 11, 12, 13, 14, 15, 1, 0, 7, 8, 9)
    words = [0] * 25
    words[0:2] = [frame, point]
    for index, value in zip(MODULE.NATIVE_COMPARE_INDEXES, values):
        words[index] = value
    words[11] = 1
    return struct.pack("<25I", *words)

class CompareGameModeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = pathlib.Path(self.temp.name)
        self.reference_path = self.directory / "original.jsonl"
        self.native_path = self.directory / "native.bin"
        self.output_path = self.directory / "report.json"

    def tearDown(self):
        self.temp.cleanup()

    def write_reference(self, rows):
        with self.reference_path.open("w", encoding="utf-8") as stream:
            stream.write(json.dumps({"schema": MODULE.SCHEMA, "format": "JSONL"}) + "\n")
            for row in rows:
                stream.write(json.dumps(row) + "\n")

    def write_native(self, records):
        self.native_path.write_bytes(b"".join(records))

    def test_exact_pre_and_post_mapping_and_unsupported_fields(self):
        first = observer_row(0)
        second = observer_row(1, phase=6, child=0, linked=0x100100, pending=(4, 5, 6))
        self.write_reference([first, second])
        self.write_native([
            native_record(104376, 0, expected(first)),
            native_record(104376, 1, expected(second)),
        ])
        report = MODULE.compare(MODULE.load_reference(self.reference_path),
                                MODULE.load_native(self.native_path, 104376), 104376)
        self.assertTrue(report["equal"])
        self.assertEqual(report["compared_records"], 2)
        self.assertEqual(report["difference_words"], 0)
        self.assertEqual(report["checks"][0]["mapping"]["original_parent_entry_frame"], 104376)
        self.assertEqual(report["checks"][1]["mapping"]["original_parent_entry_frame"], 104377)
        self.assertEqual([item["name"] for item in report["unsupported_checks"]],
                         ["field_present", "fade_current", "fade_target", "fade_step", "capture_alpha",
                          "capture_target", "frozen_frame", "history_frame", "clear_rgb_r", "clear_rgb_g", "clear_rgb_b"])

    def test_corrupt_value_reports_counts_and_first_difference(self):
        row = observer_row(0)
        values = list(expected(row))
        values[0] ^= 1
        self.write_reference([row])
        self.write_native([native_record(104376, 0, values)])
        report = MODULE.compare(MODULE.load_reference(self.reference_path),
                                MODULE.load_native(self.native_path, 104376), 104376)
        self.assertFalse(report["equal"])
        self.assertEqual(report["difference_words"], 1)
        self.assertEqual(report["difference_counts_by_field"], {"phase": 1})
        self.assertEqual(report["first_difference"]["difference"]["field"], "phase")

    def test_missing_original_frame_is_reported_and_fails_comparison(self):
        self.write_reference([observer_row(1)])
        self.write_native([native_record(104376, 0)])
        report = MODULE.compare(MODULE.load_reference(self.reference_path),
                                MODULE.load_native(self.native_path, 104376), 104376)
        self.assertFalse(report["equal"])
        self.assertEqual(report["missing_original_records"], 1)
        self.assertEqual(report["missing"][0]["original_parent_entry_frame"], 104376)

    def test_duplicate_original_parent_entry_fails_closed(self):
        row = observer_row(0)
        self.write_reference([row, row])
        with self.assertRaisesRegex(MODULE.ComparisonError, "Duplicate original"):
            MODULE.load_reference(self.reference_path)

    def test_invalid_native_extent_duplicate_and_order_fail_closed(self):
        self.write_native([b"x"])
        with self.assertRaisesRegex(MODULE.ComparisonError, "exact 100-byte"):
            MODULE.load_native(self.native_path, 104376)
        record = native_record(104376, 0)
        self.write_native([record, record])
        with self.assertRaisesRegex(MODULE.ComparisonError, "Duplicate native"):
            MODULE.load_native(self.native_path, 104376)
        self.write_native([native_record(104377, 0), native_record(104376, 1)])
        with self.assertRaisesRegex(MODULE.ComparisonError, "not strictly monotonic"):
            MODULE.load_native(self.native_path, 104376)

    def test_cli_returns_difference_and_input_failure_statuses(self):
        row = observer_row(0)
        self.write_reference([row])
        mismatched = list(expected(row))
        mismatched[0] ^= 1
        self.write_native([native_record(104376, 0, mismatched)])
        result = subprocess.run(
            [sys.executable, str(TOOL), "--reference", str(self.reference_path), "--native", str(self.native_path),
             "--anchor", "104376", "--output", str(self.output_path)], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(json.loads(self.output_path.read_text())["equal"])

        bad_output = self.directory / "bad.json"
        self.native_path.write_bytes(b"bad")
        failed = subprocess.run(
            [sys.executable, str(TOOL), "--reference", str(self.reference_path), "--native", str(self.native_path),
             "--anchor", "104376", "--output", str(bad_output)], capture_output=True, text=True
        )
        self.assertEqual(failed.returncode, 2)
        self.assertIn("exact 100-byte records", failed.stderr)
        self.assertFalse(bad_output.exists())

if __name__ == "__main__":
    unittest.main()
