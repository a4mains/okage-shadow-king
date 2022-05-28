import hashlib
import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / 'tools' / 'verify_battle_stage.py'
SPEC = importlib.util.spec_from_file_location('verify_battle_stage', SCRIPT)
v = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(v)

def pack_words(values):
    return struct.pack('<' + 'I' * len(values), *values)

def wrap(data):
    return {'bytes': len(data), 'raw_hex': data.hex()}

def make_fixture():
    stage = bytearray(v.STG_SIZE)
    struct.pack_into('<I', stage, 0, 0x00475453)
    struct.pack_into('<II', stage, 8, v.SETTING_BEGIN, v.SETTING_COUNT)
    struct.pack_into('<II', stage, 16, v.CAMERA_BEGIN, v.CAMERA_COUNT)
    struct.pack_into('<II', stage, 24, v.ENTRANCE_BEGIN, v.ENTRANCE_COUNT)
    for i in range(v.SETTING_COUNT):
        struct.pack_into('<5I', stage, v.SETTING_BEGIN + i * 20, i, i + 10, i + 20, i + 30, i + 40)
    for i in range(v.CAMERA_COUNT):
        struct.pack_into('<7I', stage, v.CAMERA_BEGIN + i * 28, *range(i * 7, i * 7 + 7))
    for i in range(v.CAMERA_SETTING_COUNT):
        struct.pack_into('<7I', stage, v.CAMERA_SETTING_BEGIN + i * 28, *range(100 + i * 7, 107 + i * 7))
    stage = bytes(stage)
    v.STG_SHA256 = hashlib.sha256(stage).hexdigest()

    events = v.expected_event_keys()
    rows = []
    for seq, (event, index) in enumerate(events):
        row = {'sequence': seq, 'event': event, 'setting_index': index}
        for key, size in [('root_header', 0x50), ('resource_header', 0x48),
                          ('stg_table_globals', 20), ('persistent_stg_prefix', 0x278),
                          ('persistent_cameras', 0x348), ('persistent_entrances', 0x348),
                          ('persistent_camera_settings', 0x348), ('projection_globals', 0x40),
                          ('field_projection', 0x84), ('current_projection_descriptor', 0x80),
                          ('effect_projection', 16), ('light_dirty_directions_colors', 0x88),
                          ('fog', 4), ('clear', 4), ('setting_4d_signed', 4),
                          ('filter_pointer', 4), ('filter_slots', 0x120)]:
            row[key] = wrap(bytes(size))
        # Stable synthetic state words make all supported comparisons deterministic.
        row['projection_globals'] = wrap(pack_words(list(range(16))))
        row['field_projection'] = wrap(pack_words(list(range(33))))
        row['light_dirty_directions_colors'] = wrap(bytes(8) + pack_words(list(range(12))) +
                                                      pack_words(list(range(16))) + pack_words(list(range(4))))
        row['effect_projection'] = wrap(pack_words([11, 12, 13, 14]))
        row['fog'] = wrap(pack_words([0xAB123456]))
        row['clear'] = wrap(pack_words([0x12345678]))
        row['setting_4d_signed'] = wrap(pack_words([0xffffffff]))
        slots = bytearray(0x120)
        for slot in range(9):
            slots[slot * 32:slot * 32 + 16] = pack_words([slot] * 4)
        row['filter_slots'] = wrap(bytes(slots))
        row['persistent_stg_prefix'] = wrap(stage[:v.HEADER_SIZE])
        row['persistent_cameras'] = wrap(stage[v.CAMERA_BEGIN:v.CAMERA_BEGIN + v.CAMERA_COUNT * 28] + bytes(0x348 - v.CAMERA_COUNT * 28))
        row['persistent_camera_settings'] = wrap(stage[v.CAMERA_SETTING_BEGIN:v.ENTRANCE_BEGIN] + bytes(0x348 - (v.ENTRANCE_BEGIN - v.CAMERA_SETTING_BEGIN)))
        resource = bytearray(0x48)
        struct.pack_into('<I', resource, 0x38, int(event != 'commit_complete_before_ui'))
        struct.pack_into('<I', resource, 0x44, int(event not in ('effect_projection_sync_entry', 'commit_complete_before_ui')))
        row['resource_header'] = wrap(bytes(resource))
        if event == 'commit_entry':
            row['stg_preimage'] = wrap(stage)
        if event in ('setting_entry', 'setting_return'):
            row['setting'] = wrap(stage[v.SETTING_BEGIN + index * 20:v.SETTING_BEGIN + (index + 1) * 20])
        rows.append(row)

    header = {'schema': v.REFERENCE_SCHEMA, 'elf_sha256': v.ELF_SHA256,
              'stg_sha256': v.STG_SHA256, 'frame_convention': 'synthetic'}
    finish = {'event': 'finish', 'commits_completed': 1, 'settings_completed': 18, 'rows': 40}
    reference_rows = [header, *rows, finish]
    native_rows = []
    for row in rows:
        field = struct.unpack('<33I', bytes.fromhex(row['field_projection']['raw_hex']))
        projection = list(struct.unpack('<16I', bytes.fromhex(row['projection_globals']['raw_hex'])))[:15]
        effects = list(struct.unpack('<4I', bytes.fromhex(row['effect_projection']['raw_hex'])))
        light = bytes.fromhex(row['light_dirty_directions_colors']['raw_hex'])
        slots = bytes.fromhex(row['filter_slots']['raw_hex'])
        resource = bytes.fromhex(row['resource_header']['raw_hex'])
        n = {'event': row['event'], 'setting_index': row['setting_index'], 'projection': projection,
             'effects': [effects[3], effects[0], effects[1], effects[2]],
             'directions': list(struct.unpack_from('<12I', light, 8)),
             'colors': list(struct.unpack_from('<16I', light, 0x38)),
             'base': list(struct.unpack_from('<4I', light, 0x78)),
             'sky': list(field[0x50 // 4:0x60 // 4]),
             'shadow_direction': list(field[0x60 // 4:0x70 // 4]),
             'shadow_tint': list(field[0x78 // 4:0x84 // 4]),
             'shadow_threshold': field[0x44 // 4], 'bucket_shift': field[0x10 // 4],
             'bucket_base': field[0x0c // 4], 'selector': field[0x40 // 4],
             'cached_depth': [field[0x30 // 4], field[0x34 // 4]],
             'fog_rgb': struct.unpack('<I', bytes.fromhex(row['fog']['raw_hex']))[0] & 0xffffff,
             'clear_word': struct.unpack('<I', bytes.fromhex(row['clear']['raw_hex']))[0],
             'signed_setting': 0xffffffff,
             'held': struct.unpack_from('<I', resource, 0x44)[0] != 0,
             'suppression': struct.unpack_from('<I', resource, 0x38)[0] != 0,
             'filters': [list(struct.unpack_from('<4I', slots, i * 32)) if i not in (1, 2) else None for i in range(9)]}
        if row['event'] in ('setting_entry', 'setting_return'):
            setting = bytes.fromhex(row['setting']['raw_hex'])
            n['kind'] = struct.unpack_from('<I', setting)[0]
            n['value_bits'] = list(struct.unpack_from('<4I', setting, 4))
        native_rows.append(n)
    native = {'schema': v.NATIVE_SCHEMA, 'frame': 99, 'complete': True, 'error': '',
              'owned_stage_hex': stage.hex(), 'header_hex': stage[:v.HEADER_SIZE].hex(),
              'cameras': [list(struct.unpack_from('<7I', stage, v.CAMERA_BEGIN + i * 28)) for i in range(8)],
              'camera_settings': [list(struct.unpack_from('<7I', stage, v.CAMERA_SETTING_BEGIN + i * 28)) for i in range(9)],
              'entrances': [], 'snapshots': native_rows}
    return reference_rows, native

class BattleStageComparisonTests(unittest.TestCase):
    def setUp(self):
        self.reference_rows, self.native = make_fixture()
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.ref_path = self.root / 'reference.jsonl'
        self.native_path = self.root / 'native.json'
        self.write_inputs()

    def tearDown(self):
        self.temp.cleanup()

    def write_inputs(self):
        self.ref_path.write_text(''.join(json.dumps(x) + '\n' for x in self.reference_rows))
        self.native_path.write_text(json.dumps(self.native))

    def test_exact_synthetic_trace_passes(self):
        result = v.compare(v.load_reference(self.ref_path), self.native)
        self.assertEqual(result['differences'], [])
        self.assertEqual(result['counts']['matched_snapshots'], 40)
        self.assertGreater(result['compared']['persistent_bytes'], 0)

    def test_field_divergence_is_retained_with_first_difference(self):
        self.native['snapshots'][4]['clear_word'] ^= 1
        result = v.compare(v.load_reference(self.ref_path), self.native)
        self.assertEqual(result['differences'][0]['path'], 'snapshot[setting_return:1].clear_word')

    def test_missing_native_record_fails_without_shifting(self):
        self.native['snapshots'].pop(2)
        result = v.compare(v.load_reference(self.ref_path), self.native)
        self.assertEqual(result['counts']['matched_snapshots'], 39)
        self.assertIn('snapshots.event_schedule', [x['path'] for x in result['differences']])

    def test_duplicate_original_sequence_is_rejected(self):
        self.reference_rows[2]['sequence'] = 0
        self.write_inputs()
        with self.assertRaises(v.Invalid):
            v.load_reference(self.ref_path)

    def test_native_incomplete_is_failure(self):
        self.native['complete'] = False
        result = v.compare(v.load_reference(self.ref_path), self.native)
        self.assertFalse(result['complete'])
        self.assertEqual(result['first_difference']['path'], 'native.complete')

    def test_cli_writes_mismatch_report_and_returns_one(self):
        self.native['snapshots'][0]['clear_word'] ^= 1
        self.write_inputs()
        output = self.root / 'comparison.json'
        status = v.main(['--reference', str(self.ref_path), '--native', str(self.native_path),
                         '--output', str(output)])
        self.assertEqual(status, 1)
        report = json.loads(output.read_text())
        self.assertEqual(report['first_difference']['path'], 'snapshot[commit_entry:0].clear_word')

    def test_excluded_fog_high_byte_and_filter_opaque_tail_are_ignored(self):
        row = self.reference_rows[1]
        fog = bytearray.fromhex(row['fog']['raw_hex'])
        fog[3] = 0x11
        row['fog'] = wrap(bytes(fog))
        slots = bytearray.fromhex(row['filter_slots']['raw_hex'])
        slots[20] = 0x99
        row['filter_slots'] = wrap(bytes(slots))
        self.write_inputs()
        result = v.compare(v.load_reference(self.ref_path), self.native)
        self.assertNotIn('snapshot[commit_entry:0].fog_rgb', [x['path'] for x in result['differences']])
        self.assertNotIn('snapshot[commit_entry:0].filters[0]', [x['path'] for x in result['differences']])

    def test_cli_rejects_existing_output_and_bad_hex(self):
        self.native['owned_stage_hex'] = 'xyz'
        self.write_inputs()
        self.assertEqual(v.main(['--reference', str(self.ref_path), '--native', str(self.native_path),
                                 '--output', str(self.root / 'result.json')]), 2)
        out = self.root / 'existing.json'
        out.write_text('{}')
        self.assertEqual(v.main(['--reference', str(self.ref_path), '--native', str(self.native_path),
                                 '--output', str(out)]), 2)

if __name__ == '__main__':
    unittest.main()
