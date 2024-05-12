import json
from pathlib import Path
import struct
import tempfile
import unittest

from tools.compare_cold_start import (
    ComparisonError,
    make_report,
    load_game_mode,
    load_source,
)

def hx(value):
    return f"0x{value:08x}"

def source_parent(frame, phase=0, current=8):
    return {
        "frame": frame,
        "pc": "0x00124818",
        "parent_phase20": hx(phase),
        "parent_current40": hx(current),
        "parent_arg44": hx(3),
        "parent_arg4c": hx(0),
        "parent_request3c": hx(0),
        "parent_queued48": hx(0),
        "parent_queued50": hx(0),
        "parent_child38": hx(0),
        "parent_linked_first18": hx(0),
        "pending_24f740": hx(0),
        "pending_24f744": hx(0),
        "pending_24f748": hx(0),
    }

def source_callback(frame, selector, counter=0, alpha=0, rate=0, image=0,
                    storage_type=2, storage_value=8192, capacity=1024, continuation=0):
    return {
        "frame": frame,
        "pc": "0x0012c8f8",
        "mode8_child": {
            "selector_24": hx(selector), "counter_30": hx(counter),
            "alpha_3c": hx(alpha), "rate_40": hx(rate), "image_44": hx(image),
        },
        "storage_device": {
            "type_2597a8": hx(storage_type), "value_2597ac": hx(storage_value),
            "capacity_2597b0": hx(capacity), "continuation_2597ca": hx(continuation),
        },
    }

def game_row(frame, point, *, phase=0, current=8, arg1=3):
    row = [0] * 25
    row[0], row[1] = frame, point
    row[2], row[3], row[4], row[5] = phase, current, arg1, 0
    row[6], row[7], row[8], row[9], row[10] = 0, 0, 0, 0, 0
    row[12], row[13], row[14] = 0, 0, 0
    return tuple(row)

def startup_row(frame, selector, counter=0, alpha=0, rate=0, image=0,
                storage_state=0x687, storage_complete=1, storage_type=2, free_capacity=4096):
    return (frame, selector, counter, alpha, rate, image, storage_state,
            storage_complete, storage_type, free_capacity, frame, 0x12345678)

def title_row(frame, selector, counter=0, alpha=0, rate=0):
    row = [0] * 43
    row[0], row[1], row[2], row[3] = frame, selector, 10, counter
    row[5], row[6] = alpha, rate
    return tuple(row)

class ColdStartComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = self.root / "native.png"
        self.reference = self.root / "mode.jsonl"
        self._write_fixture()

    def _write_fixture(self):
        rows = [
            {"schema": "okage-game-mode-observer-v1", "elf_sha256": "a" * 64},
            *[source_parent(frame, phase=1 if frame == 76 else 0)
              for frame in [75, 76, *range(78, 100)]],
            source_callback(81, 3, counter=0, storage_type=2, storage_value=8192,
                            capacity=1024, continuation=0x687),
            source_callback(82, 0, counter=0, alpha=0, rate=0x3CCCCCCD, image=0,
                            capacity=1024, continuation=0x69A),
            source_callback(83, 0, counter=1, alpha=0x3CCCCCCD, rate=0x3CCCCCCD, image=0,
                            capacity=1024, continuation=0x69A),
            source_callback(90, 1, counter=0, alpha=0, rate=0),
        ]
        self.reference.write_text("\n".join(json.dumps(row) for row in rows) + "\n")
        (self.root / "run.json").write_text(json.dumps({
            "command": ["oracle", "disc.cue", "100", "--game-mode", str(self.reference)],
            "exit_code": 0,
        }))

        game_rows = []
        for frame in (1, 2, 4):
            source_frame = 75 + frame - 1
            phase = 1 if source_frame == 76 else 0
            game_rows.append(game_row(frame, 0, phase=phase))
            game_rows.append(game_row(frame, 1, phase=phase))
        (Path(str(self.prefix) + ".game-mode.bin")).write_bytes(
            b"".join(struct.pack("<25I", *row) for row in game_rows))
        startup = [
            startup_row(5, 3, storage_state=0x687, free_capacity=999),
            startup_row(6, 3, storage_state=0x687, free_capacity=999),
            startup_row(7, 0, counter=1, alpha=0x3CCCCCCD, rate=0x3CCCCCCD, image=0,
                        storage_state=0x69A, free_capacity=999),
            startup_row(8, 0, counter=2, alpha=0x3D4CCCCD, rate=0x3CCCCCCD, image=0,
                        storage_state=0x69A, free_capacity=999),
        ]
        (Path(str(self.prefix) + ".startup.bin")).write_bytes(
            b"".join(struct.pack("<12I", *row) for row in startup))
        (Path(str(self.prefix) + ".title-state.bin")).write_bytes(
            struct.pack("<43I", *title_row(15, 1, counter=0, alpha=0, rate=0)))
        (Path(str(self.prefix) + ".json")).write_text(json.dumps({"frames": 15}))

    def test_fixed_anchor_point0_and_source_gaps_are_explicit(self):
        report, _ = make_report(self.reference, self.prefix)
        parent = report["fixed_anchor_parent_entry"]
        self.assertEqual(parent["mapping"]["anchor_source_parent_entry_frame"], 75)
        self.assertEqual(parent["mapping"]["anchor_native_frame"], 1)
        self.assertEqual(parent["compared_records"], 3)
        self.assertEqual(parent["missing_source_frame_ranges"], [{"start": 77, "end": 77, "count": 1}])
        self.assertEqual(parent["missing_native_point0_frame_ranges"], [
            {"start": 3, "end": 3, "count": 1},
            {"start": 5, "end": 15, "count": 11},
        ])
        self.assertEqual(parent["source_entries_without_native_point0"], 11)
        self.assertEqual(parent["source_entries_without_native_point0_ranges"],
                         [{"start": 79, "end": 89, "count": 11}])
        self.assertEqual(parent["source_entries_within_original_interval_outside_native_capture"], 10)
        self.assertEqual(report["original_interval"]["requested_vblanks_stop_boundary"], 100)
        self.assertFalse(report["semantic_equal"])

    def test_callbacks_map_to_previous_completed_tick_and_keep_capacity_only_as_evidence(self):
        report, _ = make_report(self.reference, self.prefix)
        callbacks = report["mode8_callbacks"]
        startup = callbacks["startup"]["pairs"]
        title = callbacks["title"]["pairs"]
        self.assertEqual([(p["source_callback_frame"], p["native_completed_frame"]) for p in startup],
                         [(81, 6), (82, 7), (83, 8)])
        self.assertEqual([(p["source_callback_frame"], p["native_completed_frame"]) for p in title], [(90, 15)])
        self.assertTrue(title[0]["equal"])
        self.assertEqual(startup[1]["storage_observations"]["source"]["free_capacity"], 1024)
        self.assertEqual(startup[1]["storage_observations"]["native"]["free_capacity"], 999)
        self.assertFalse(any(d["field"] == "storage_capacity" for d in startup[1]["differences"]))

    def test_logo_ordinal_is_separate_and_does_not_fill_source_gaps(self):
        report, _ = make_report(self.reference, self.prefix)
        callbacks = report["mode8_callbacks"]
        ordinal = callbacks["ordinal_logo_progression"]
        self.assertIn("not global timing/parity", ordinal["scope"])
        self.assertEqual(ordinal["source_samples"], 2)
        self.assertEqual(ordinal["native_samples"], 2)
        self.assertEqual(callbacks["source_logo_selector0_frame_gaps"], [])
        self.assertEqual(callbacks["startup"]["missing_source_frame_ranges"], [{"start": 80, "end": 80, "count": 1}])
        self.assertEqual(ordinal["paired_samples"], 1)
        self.assertEqual(ordinal["unpaired_source_endpoints"][0]["counter"], 0)
        self.assertEqual(ordinal["unpaired_native_endpoints"][0]["counter"], 2)

    def test_first_native_frame_must_be_tick_one(self):
        path = Path(str(self.prefix) + ".game-mode.bin")
        rows = [game_row(2, 0)]
        path.write_bytes(struct.pack("<25I", *rows[0]))
        with self.assertRaisesRegex(ComparisonError, "fixed anchor requires tick 1"):
            make_report(self.reference, self.prefix)

    def test_source_header_and_binary_record_extents_fail_closed(self):
        header, _, parents, _ = load_source(self.reference)
        self.assertEqual(header["schema"], "okage-game-mode-observer-v1")
        self.assertEqual(len(parents), 24)
        path = Path(str(self.prefix) + ".startup.bin")
        path.write_bytes(b"truncated")
        with self.assertRaisesRegex(ComparisonError, "expected 48-byte records"):
            make_report(self.reference, self.prefix)

    def test_capture_metadata_preserves_suppressed_native_tail(self):
        path = Path(str(self.prefix) + ".game-mode.bin")
        rows = [game_row(1, 0), game_row(2, 0)]
        path.write_bytes(b"".join(struct.pack("<25I", *row) for row in rows))
        report, _ = make_report(self.reference, self.prefix)
        parent = report["fixed_anchor_parent_entry"]
        self.assertEqual(parent["missing_native_point0_frame_ranges"],
                         [{"start": 3, "end": 15, "count": 13}])
        self.assertEqual(report["original_interval"]["fixed_anchor_overlap_native_frames"], [1, 15])

    def test_different_first_parent_does_not_refit_anchor(self):
        data = [json.loads(line) for line in self.reference.read_text().splitlines()]
        data[1]["frame"] = 74
        self.reference.write_text("\n".join(json.dumps(row) for row in data) + "\n")
        with self.assertRaisesRegex(ComparisonError, "requires fixed frame 75"):
            make_report(self.reference, self.prefix)

    def test_native_trace_beyond_capture_frame_count_fails_closed(self):
        Path(str(self.prefix) + ".json").write_text(json.dumps({"frames": 3}))
        with self.assertRaisesRegex(ComparisonError, "beyond capture metadata frames"):
            make_report(self.reference, self.prefix)

if __name__ == "__main__":
    unittest.main()
