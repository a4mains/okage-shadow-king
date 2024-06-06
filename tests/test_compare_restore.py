import importlib.util
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).parents[1]
TOOL = ROOT / "tools/compare_restore.py"
SPEC = importlib.util.spec_from_file_location("compare_restore", TOOL)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)

def source_restore_row(frame, *, state=3, selected=2, completed=0, previous_opacity=0x3F800000,
                       repeat_delay=4, confirmation=1, dead=False):
    return {
        "frame": frame,
        "pc": MODULE.RESTORE_CALLBACK_PC,
        "restore": {
            "edge": "second_callback_entry",
            "global_1f89a8": f"0x{state:08x}",
            "global_1f89ac": f"0x{selected:08x}",
            "global_24ecc0": f"0x{completed:08x}",
            "global_24ecc4": f"0x{previous_opacity:08x}",
            "global_24ecd8": f"0x{repeat_delay:08x}",
            "global_24ee08": f"0x{confirmation:08x}",
            "manager_flags1c": f"0x{0x80000000 if dead else 0:08x}",
        },
    }

def source_parent_row(frame, phase=7):
    return {"frame": frame, "pc": MODULE.PARENT_ENTRY_PC,
            "parent_phase20": f"0x{phase:08x}"}

def native_restore_row(frame, *, state=3, selected=2, completed=0, previous_opacity=0x3F800000,
                       repeat_delay=4, confirmation=1, dead=0, present=1, opacity=0):
    row = [0] * MODULE.RESTORE_WORDS
    row[0] = frame
    row[2] = present
    row[3] = dead
    row[4] = completed
    row[5] = state
    row[6] = selected
    row[7] = confirmation
    row[10] = opacity
    row[11] = previous_opacity
    row[12] = repeat_delay
    return struct.pack(f"<{MODULE.RESTORE_WORDS}I", *row)

def native_parent_row(frame, phase=7, point=0):
    row = [0] * MODULE.GAME_MODE_WORDS
    row[0] = frame
    row[1] = point
    row[2] = phase
    return struct.pack(f"<{MODULE.GAME_MODE_WORDS}I", *row)

class CompareRestoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.source = self.root / "source.jsonl"
        self.restore = self.root / "restore.bin"
        self.mode = self.root / "game-mode.bin"
        self.output = self.root / "report.json"

    def tearDown(self):
        self.temp.cleanup()

    def write_source(self, rows, *, schema=MODULE.SOURCE_SCHEMA, elf=MODULE.SOURCE_ELF_SHA256):
        with self.source.open("w", encoding="utf-8") as stream:
            stream.write(json.dumps({"schema": schema, "elf_sha256": elf}) + "\n")
            for row in rows:
                stream.write(json.dumps(row) + "\n")

    def compare(self, source_stop=3, native_offset=1200):
        source_restore, source_parent = MODULE.load_source(self.source, source_stop)
        native_restore = MODULE.load_native_restore(self.restore)
        native_parent = MODULE.load_native_game_mode(self.mode)
        return MODULE.compare(source_restore, source_parent, native_restore, native_parent,
                              native_offset=native_offset, source_stop=source_stop)

    def test_fixed_anchor_compares_only_documented_state_and_parent_phase(self):
        rows = [source_restore_row(0), source_parent_row(0), source_restore_row(1, dead=True), source_parent_row(1, 8)]
        self.write_source(rows)
        self.restore.write_bytes(native_restore_row(1200) + native_restore_row(1201, dead=1))
        # point 1 is deliberately different and must not be used for parent-entry comparison.
        self.mode.write_bytes(native_parent_row(1200) + native_parent_row(1200, phase=99, point=1) +
                              native_parent_row(1201, phase=8))
        report = self.compare(source_stop=3)
        self.assertEqual(report["restore_state"]["compared_records"], 2)
        self.assertEqual(report["restore_state"]["difference_words"], 0)
        self.assertEqual(report["restore_state"]["excluded_native_fields"][0]["name"], "opacity")
        self.assertEqual(report["parent_phase"]["compared_records"], 2)
        self.assertEqual(report["parent_phase"]["difference_records"], 0)
        self.assertEqual(report["restore_state"]["checks"][0]["native_frame"], 1200)
        self.assertEqual(report["alignment"]["native_offset"], 1200)
        self.assertEqual(report["restore_state"]["both_absent_after_last_observation_source_frames"], [2])
        self.assertEqual(report["restore_state"]["both_absent_with_observed_retirement_evidence_source_frames"], [])

    def test_reports_field_mismatches_and_first_difference(self):
        self.write_source([source_restore_row(0, selected=2), source_parent_row(0, phase=7)])
        self.restore.write_bytes(native_restore_row(1200, selected=9))
        self.mode.write_bytes(native_parent_row(1200, phase=8))
        report = self.compare(source_stop=1)
        self.assertEqual(report["restore_state"]["difference_counts_by_field"], {"selected": 1})
        self.assertEqual(report["restore_state"]["first_difference"]["difference"]["field"], "selected")
        self.assertEqual(report["parent_phase"]["difference_records"], 1)
        self.assertFalse(report["component_values_equal_when_paired"])

    def test_missing_rows_are_not_filled_and_both_retirement_tails_are_separate(self):
        self.write_source([source_restore_row(0), source_parent_row(0)])
        self.restore.write_bytes(native_restore_row(1200))
        self.mode.write_bytes(native_parent_row(1200))
        report = self.compare(source_stop=3)
        state = report["restore_state"]
        self.assertEqual(state["missing_source_frames"], [1, 2])
        self.assertEqual(state["missing_native_or_retired_source_frames"], [1, 2])
        self.assertEqual(state["both_absent_after_last_observation_source_frames"], [1, 2])
        self.assertEqual(state["both_absent_with_observed_retirement_evidence_source_frames"], [])
        parent = report["parent_phase"]
        self.assertEqual(parent["missing_source_frames"], [1, 2])
        self.assertEqual(parent["missing_native_source_frames"], [1, 2])

    def test_retirement_evidence_requires_dead_flags_and_native_absence_record(self):
        self.write_source([source_restore_row(0, dead=True)])
        self.restore.write_bytes(native_restore_row(1200, dead=1) + native_restore_row(1201, dead=1, present=0))
        self.mode.write_bytes(native_parent_row(1200))
        report = self.compare(source_stop=3)
        state = report["restore_state"]
        self.assertEqual(state["last_source_manager_dead_flag"], 1)
        self.assertEqual(state["last_native_manager_dead_flag"], 1)
        self.assertEqual(state["both_absent_with_observed_retirement_evidence_source_frames"], [1])
        self.assertEqual(state["both_absent_after_last_observation_source_frames"], [2])

    def test_zero_paired_records_are_not_reported_equal(self):
        report = MODULE.compare({}, {}, {}, {}, native_offset=1200, source_stop=1)
        self.assertFalse(report["restore_state"]["values_equal_when_paired"])
        self.assertFalse(report["parent_phase"]["values_equal_when_paired"])
        self.assertFalse(report["component_values_equal_when_paired"])

    def test_fixed_offset_required_and_source_schema_hash_validated(self):
        self.write_source([source_restore_row(0)], schema="wrong")
        with self.assertRaisesRegex(MODULE.ComparisonError, "schema"):
            MODULE.load_source(self.source, 1)
        self.write_source([source_restore_row(0)], elf="0" * 64)
        with self.assertRaisesRegex(MODULE.ComparisonError, "ELF hash"):
            MODULE.load_source(self.source, 1)
        with self.assertRaisesRegex(MODULE.ComparisonError, "requires --native-offset 1200"):
            MODULE.compare({}, {}, {}, {}, native_offset=1201, source_stop=1)

    def test_rejects_malformed_record_extents_and_duplicates(self):
        self.restore.write_bytes(b"x")
        with self.assertRaisesRegex(MODULE.ComparisonError, "exact 3912-byte"):
            MODULE.load_native_restore(self.restore)
        self.mode.write_bytes(native_parent_row(1200) + native_parent_row(1200))
        with self.assertRaisesRegex(MODULE.ComparisonError, "not strictly increasing"):
            MODULE.load_native_game_mode(self.mode)
        self.write_source([source_restore_row(0), source_restore_row(0)])
        with self.assertRaisesRegex(MODULE.ComparisonError, "Duplicate source Restore"):
            MODULE.load_source(self.source, 1)

    def test_cli_requires_explicit_captured_interval_and_offset(self):
        self.write_source([source_restore_row(0)])
        self.restore.write_bytes(native_restore_row(1200))
        self.mode.write_bytes(native_parent_row(1200))
        result = subprocess.run([sys.executable, str(TOOL), "--source", str(self.source),
                                 "--native-restore", str(self.restore), "--native-game-mode", str(self.mode),
                                 "--native-offset", "1201", "--source-stop", "300", "--output", str(self.output)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--native-offset must be the fixed value 1200", result.stderr)
        self.assertFalse(self.output.exists())

if __name__ == "__main__":
    unittest.main()
