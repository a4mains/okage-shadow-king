import copy
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_ui_window import (ELF_SHA256, ENTRY_EVENT, ENTRY_PC, CONTINUATION_PC,
                              EXCLUDED_OFFSETS, NATIVE_SCHEMA, REFERENCE_SCHEMA,
                              STREAMS, WINDOW_BYTES, WINDOW_WORDS, compare,
                              load_reference, Invalid)

TOOL = Path(__file__).resolve().parents[1] / 'tools' / 'verify_ui_window.py'

def raw_wrapper(address, data):
    return {'address': address, 'bytes': len(data), 'raw_hex': data.hex()}

def source_table():
    data = bytearray(0xe4)
    for index, (stream, cap_a, cap_b, layer_type, callback) in enumerate(STREAMS):
        base = index * 112
        struct.pack_into('<4I', data, base, stream, 40, 6, 2)
        struct.pack_into('<I', data, base + 0x54, 0x1fdba8)
        struct.pack_into('<III', data, base + 0x58, (0x163fb8 if index == 0 else 0x168228), callback, layer_type)
        struct.pack_into('<III', data, base + 0x64, cap_a, cap_b, 0)
    return bytes(data)

def window_bytes(slot):
    data = bytearray(WINDOW_BYTES)
    struct.pack_into('<I', data, 4, slot)
    struct.pack_into('<I', data, 0x14, 0x40)
    struct.pack_into('<III', data, 0x250, 6, 6, 6)
    struct.pack_into('<II', data, 0x3c00, STREAMS[slot][1], STREAMS[slot][2])
    return bytes(data)

def native_from_windows(raw_windows):
    windows = []
    excluded = {offset // 4 for offset in EXCLUDED_OFFSETS}
    for slot, data in enumerate(raw_windows):
        words = list(struct.unpack('<' + 'I' * WINDOW_WORDS, data))
        for index in excluded:
            words[index] = None
        windows.append({'slot': slot, 'words': words})
    return {
        'schema': NATIVE_SCHEMA,
        'scope': '198e38 and window configuration only; empty incoming streams and atlas, slots0/1; no layer constructors',
        'windows': windows,
    }

def make_reference():
    header = {'schema': REFERENCE_SCHEMA, 'elf_sha256': ELF_SHA256}
    root = 0x1dbd0c0
    sp = 0x1ffeb70
    pointers = [0x8a39d0, 0xa7a150]
    windows = [window_bytes(0), window_bytes(1)]
    entry_slots = [{'slot': i, 'pointer': 0} for i in range(8)]
    complete_slots = []
    for i in range(8):
        if i < 2:
            complete_slots.append({'slot': i, 'pointer': pointers[i],
                                   'raw': raw_wrapper(pointers[i], windows[i])})
        else:
            complete_slots.append({'slot': i, 'pointer': 0})
    manager = bytearray(0xef0)
    struct.pack_into('<III', manager, 0xe40, 2, 0, 0)
    struct.pack_into('<I', manager, 0xe94, 1)
    layers = []
    for index, (stream, _, _, layer_type, callback) in enumerate(STREAMS):
        layer = bytearray(0xe0)
        struct.pack_into('<I', layer, 0, index)
        struct.pack_into('<I', layer, 0x54, stream)
        struct.pack_into('<II', layer, 0x5c, callback, layer_type)
        layers.append(bytes(layer))
        manager[0x40 + index * 0xe0:0x40 + (index + 1) * 0xe0] = layer
    common = {
        'frame': 126, 'sp': sp, 'root': root, 'constructor_sp': sp,
        'fcsr': 0x01000001, 'shared_globals': {'battle_root_pointer': root},
    }
    entry = {
        **common, 'event': ENTRY_EVENT, 'sequence': 0, 'pc': ENTRY_PC,
        'ra': CONTINUATION_PC, 'post_constructor': False, 'manager': 0,
        'controller_count': 0,
        'controller_registry': raw_wrapper(0x200688, bytes(32)),
        'controller_windows': entry_slots,
        'stream_type2_written_extent': raw_wrapper(0x1fd820, bytes(20)),
        'stream_type3_written_extent': raw_wrapper(0x1fdb88, bytes(20)),
        'atlas_region': raw_wrapper(0x1fdba8, bytes(16)),
        'party_selection_counts_and_table': raw_wrapper(0x1fae18, bytes(0x120)),
        'party_selection_cache_unchanged_since_entry': True,
    }
    registry = struct.pack('<8I', pointers[0], pointers[1], 0, 0, 0, 0, 0, 0)
    complete = {
        **common, 'event': 'ui_constructor_complete_before_activation', 'sequence': 1,
        'pc': CONTINUATION_PC, 'ra': CONTINUATION_PC, 'post_constructor': True,
        'manager': 0x1dc68c0, 'controller_count': 2,
        'controller_registry': raw_wrapper(0x200688, registry),
        'controller_windows': complete_slots,
        'authored_interface_records_and_terminator': raw_wrapper(0x248100, source_table()),
        'manager_raw': raw_wrapper(0x1dc68c0, bytes(manager)),
        'layer0': raw_wrapper(0, layers[0]), 'layer1': raw_wrapper(0, layers[1]),
        'stream_type2_written_extent': raw_wrapper(0x1fd820, bytes(20)),
        'stream_type3_written_extent': raw_wrapper(0x1fdb88, bytes(20)),
        'atlas_region': raw_wrapper(0x1fdba8, bytes(16)),
        'party_selection_counts_and_table': raw_wrapper(0x1fae18, bytes(0x120)),
        'party_selection_cache_unchanged_since_entry': True,
    }
    finish = {'event': 'finish', 'constructors_entered': 1,
              'constructors_completed': 1, 'snapshots': 2}
    return [header, entry, complete, finish], native_from_windows(windows)

def write_jsonl(path, rows):
    path.write_text(''.join(json.dumps(row, separators=(',', ':')) + '\n' for row in rows))

class VerifyUiWindowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.reference_path = self.root / 'reference.jsonl'
        self.native_path = self.root / 'native.json'
        self.output_path = self.root / 'verified.json'
        self.reference, self.native = make_reference()
        write_jsonl(self.reference_path, self.reference)
        self.native_path.write_text(json.dumps(self.native))

    def tearDown(self):
        self.temp.cleanup()

    def run_cli(self, expect):
        result = subprocess.run([sys.executable, str(TOOL), '--reference', str(self.reference_path),
                                 '--native', str(self.native_path), '--output', str(self.output_path)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, expect, result.stderr + result.stdout)
        return json.loads(self.output_path.read_text()) if expect in (0, 1) and self.output_path.exists() else None

    def save_inputs(self):
        write_jsonl(self.reference_path, self.reference)
        self.native_path.write_text(json.dumps(self.native))

    def test_exact_bounded_comparison_counts_every_known_word(self):
        report = self.run_cli(0)
        self.assertEqual(report['status'], 'pass')
        self.assertEqual(report['compared_words'], 14146)
        self.assertEqual(report['compared_bytes'], 56584)
        self.assertEqual(report['difference_count'], 0)
        self.assertEqual(report['windows'][0]['window_bytes'], 0x6ec0)
        self.assertIn('not layer constructors', report['unsupported'][1])

    def test_word_mismatch_is_retained_in_report(self):
        self.native['windows'][1]['words'][100] ^= 1
        self.save_inputs()
        report = self.run_cli(1)
        self.assertEqual(report['first_difference'], report['differences'][0])
        self.assertEqual(report['difference_count'], 1)
        self.assertEqual(report['differences'][0]['slot'], 1)
        self.assertEqual(report['differences'][0]['offset'], 400)

    def test_arbitrary_null_is_rejected(self):
        self.native['windows'][0]['words'][1] = None
        self.save_inputs()
        self.assertIsNone(self.run_cli(2))

    def test_excluded_offset_must_be_null(self):
        self.native['windows'][0]['words'][0] = 0
        self.save_inputs()
        self.assertIsNone(self.run_cli(2))

    def test_truncated_window_is_rejected(self):
        self.native['windows'][0]['words'].pop()
        self.save_inputs()
        self.assertIsNone(self.run_cli(2))

    def test_missing_or_duplicate_original_snapshot_is_rejected(self):
        self.reference.pop(2)
        self.save_inputs()
        self.assertIsNone(self.run_cli(2))
        self.reference, self.native = make_reference()
        self.reference.insert(2, copy.deepcopy(self.reference[1]))
        self.save_inputs()
        self.assertIsNone(self.run_cli(2))

    def test_altered_prestate_is_rejected(self):
        raw_obj = self.reference[1]['stream_type2_written_extent']
        raw_obj['raw_hex'] = '01' + raw_obj['raw_hex'][2:]
        self.save_inputs()
        self.assertIsNone(self.run_cli(2))

    def test_wrong_capacity_binding_is_rejected(self):
        self.native['windows'][1]['words'][0x3c00 // 4] = 29
        self.save_inputs()
        report = self.run_cli(1)
        self.assertEqual(report['difference_count'], 1)
        self.assertEqual(report['differences'][0]['offset'], 0x3c00)

    def test_existing_output_is_never_clobbered(self):
        self.output_path.write_text('keep')
        self.assertIsNone(self.run_cli(2))
        self.assertEqual(self.output_path.read_text(), 'keep')

    def test_output_cannot_alias_input(self):
        result = subprocess.run([sys.executable, str(TOOL), '--reference', str(self.reference_path),
                                 '--native', str(self.native_path), '--output', str(self.native_path)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertTrue(self.native_path.exists())

if __name__ == '__main__':
    unittest.main()
