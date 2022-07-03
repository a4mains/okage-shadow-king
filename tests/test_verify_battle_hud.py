import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import verify_battle_hud as verifier

def _wrapper(data, address=None):
    row = {'bytes': len(data), 'raw_hex': data.hex()}
    if address is not None:
        row['address'] = address
    return row

def fixture():
    counts = verifier.GROUP_COUNTS
    offsets = verifier.GROUP_RECORD_OFFSETS
    raw = bytearray(verifier.RAW_SOURCE_BYTES)
    runtime = bytearray(verifier.RUNTIME_SOURCE_BYTES)
    text = bytearray(verifier.TEXT_SOURCE_BYTES)
    raw_base = 0x100000
    runtime_base = 0x200000
    text_base = 0x300000
    raw[:8] = b'rxuw1000'
    struct.pack_into('<I', raw, 20, 11)
    struct.pack_into('<I', raw, 24, 0)
    struct.pack_into('<6I', raw, 28, *counts)
    struct.pack_into('<6I', raw, 52, *offsets)
    runtime_index = text_index = 0
    flat_index = 0
    for group, count in enumerate(counts):
        for item in range(count):
            widget_type = 1 if flat_index < 73 else 2
            owns_runtime = flat_index < 240
            flags = 0x40 if (widget_type <= 3 and owns_runtime and widget_type != 1) else 0
            words = [0x1000 + flat_index * 0x20 + w for w in range(17)]
            words[4] = flags
            words[5] = widget_type
            words[16] = runtime_base + runtime_index * verifier.RUNTIME_BYTES if owns_runtime else 0
            record_at = verifier.GROUPS_OFFSET + (offsets[group] + item) * verifier.RECORD_BYTES
            struct.pack_into('<17I', raw, record_at, *words)
            if owns_runtime:
                runtime_words = [0x5000 + runtime_index * 16 + w for w in range(11)]
                if widget_type == 1:
                    runtime_words[10] = text_base + text_index * verifier.TEXT_BYTES
                    encoded = f'widget-{text_index:03d}'.encode() + b'\0'
                    text[text_index * verifier.TEXT_BYTES:text_index * verifier.TEXT_BYTES + len(encoded)] = encoded
                    text_index += 1
                struct.pack_into('<11I', runtime, runtime_index * verifier.RUNTIME_BYTES, *runtime_words)
                runtime_index += 1
            flat_index += 1
    assert runtime_index == 240 and text_index == 73
    metadata_words = (87, 1, 6, raw_base + 0x4ca0, raw_base + 0x4cc0,
                      raw_base + 0x4ce0, raw_base + 0x4d10,
                      raw_base + 48, raw_base + 1440, raw_base + verifier.GROUPS_OFFSET, 0, 0)
    slot = {
        'raw_pointer': raw_base, 'runtime_pointer': runtime_base, 'text_pointer': text_base,
        'metadata': _wrapper(struct.pack('<12I', *metadata_words), 0x200750),
        'template_group_counts': _wrapper(struct.pack('<I', 11), metadata_words[3]),
        'template_group_offsets': _wrapper(struct.pack('<I', 0), metadata_words[4]),
        'widget_group_counts': _wrapper(struct.pack('<6I', *counts), metadata_words[5]),
        'widget_group_offsets': _wrapper(struct.pack('<6I', *offsets), metadata_words[6]),
        'raw_owner': _wrapper(raw, raw_base),
        'runtime_owner': _wrapper(runtime, runtime_base),
        'text_owner': _wrapper(text, text_base),
    }
    complete = {'ruw_slot0': slot,
                'shared_globals': {key: index for index, key in enumerate(verifier.GLOBAL_KEYS)}}
    entry = {'frame': 123, 'root': 0x123400}
    ref = ({'schema': verifier.REFERENCE_SCHEMA}, entry, complete, {}, entry['root'])
    native_groups = []
    raw_bytes = bytes(raw)
    runtime_bytes = bytes(runtime)
    text_bytes = bytes(text)
    for group, count in enumerate(counts):
        group_rows = []
        for item in range(count):
            at = verifier.GROUPS_OFFSET + (offsets[group] + item) * verifier.RECORD_BYTES
            words = list(struct.unpack_from('<17I', raw_bytes, at))
            pointer = words[16]
            if pointer:
                run_at = pointer - runtime_base
                run_words = list(struct.unpack_from('<11I', runtime_bytes, run_at))
                run = run_words[:10]
                if words[5] == 1:
                    text_at = run_words[10] - text_base
                    text_hex = text_bytes[text_at:text_at + verifier.TEXT_BYTES].hex()
                else:
                    text_hex = None
            else:
                run = text_hex = None
            group_rows.append({'words': words, 'runtime': run, 'text_hex': text_hex})
        native_groups.append(group_rows)
    native = {
        'schema': verifier.NATIVE_SCHEMA, 'scope': verifier.NATIVE_SCOPE,
        'game_mode': 3, 'selected': 0, 'vars_hex': bytes(verifier.VARS_BYTES).hex(),
        'globals': dict(complete['shared_globals']), 'groups': native_groups,
    }
    return ref, native

class VerifyBattleHudTests(unittest.TestCase):
    def test_exact_comparison_covers_all_supported_records(self):
        report = verifier.compare(*fixture())
        self.assertEqual(report['status'], 'pass')
        self.assertEqual(report['widget_count'], 256)
        self.assertEqual(report['compared_raw_words'], 4096)
        self.assertEqual(report['compared_runtime_words'], 2400)
        self.assertEqual(report['compared_text_bytes'], sum(len(f'widget-{i:03d}') + 1 for i in range(73)))
        self.assertEqual(report['difference_count'], 0)

    def test_reports_raw_runtime_text_and_global_differences(self):
        ref, native = fixture()
        native['groups'][0][0]['words'][0] ^= 1
        native['groups'][0][0]['runtime'][3] ^= 1
        native['groups'][0][0]['text_hex'] = (b'changed\0' + bytes(120)).hex()
        native['globals']['type2_flag_a'] = 7
        report = verifier.compare(ref, native)
        self.assertEqual(report['status'], 'fail')
        self.assertGreaterEqual(report['difference_count'], 4)
        self.assertEqual(report['first_difference']['field'], 'globals.type2_flag_a')

    def test_guest_pointers_are_excluded_but_still_require_u32_encoding(self):
        ref, native = fixture()
        native['groups'][0][0]['words'][16] = 0
        self.assertEqual(verifier.compare(ref, native)['status'], 'pass')
        native['groups'][0][0]['words'][16] = True
        with self.assertRaises(verifier.Invalid):
            verifier.compare(ref, native)

    def test_rejects_runtime_presence_mismatch_and_malformed_text(self):
        ref, native = fixture()
        native['groups'][0][0]['runtime'] = None
        with self.assertRaises(verifier.Invalid):
            verifier.compare(ref, native)
        ref, native = fixture()
        native['groups'][0][0]['text_hex'] = (b'x' * 128).hex()
        with self.assertRaises(verifier.Invalid):
            verifier.compare(ref, native)

    def test_rejects_bad_extent_parameters_globals_and_execution(self):
        ref, native = fixture()
        native['vars_hex'] = '00' * 8
        with self.assertRaises(verifier.Invalid):
            verifier.compare(ref, native)
        ref, native = fixture()
        native['game_mode'] = True
        with self.assertRaises(verifier.Invalid):
            verifier.compare(ref, native)
        ref, native = fixture()
        native['globals'].pop('ui_mode_counter')
        with self.assertRaises(verifier.Invalid):
            verifier.compare(ref, native)
        ref, native = fixture()
        native['execution'] = 'rendered'
        with self.assertRaises(verifier.Invalid):
            verifier.compare(ref, native)
        ref, native = fixture()
        native['execution'] = 'native-session'
        self.assertEqual(verifier.compare(ref, native)['native_execution'], 'native-session')

    def test_rejects_out_of_bounds_original_runtime_pointer(self):
        ref, native = fixture()
        slot = ref[2]['ruw_slot0']
        raw = bytearray.fromhex(slot['raw_owner']['raw_hex'])
        struct.pack_into('<I', raw, verifier.GROUPS_OFFSET + 64, slot['runtime_pointer'] + slot['runtime_owner']['bytes'])
        slot['raw_owner'] = _wrapper(raw, slot['raw_pointer'])
        with self.assertRaises(verifier.Invalid):
            verifier.compare(ref, native)

    def test_refuses_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'report.json'
            output.write_text('{}')
            self.assertEqual(verifier.main(['--reference', 'unused', '--native', 'unused',
                                            '--output', str(output)]), 2)
            self.assertEqual(output.read_text(), '{}')

if __name__ == '__main__':
    unittest.main()
