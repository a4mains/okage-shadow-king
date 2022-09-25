import copy
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify_battle_hud as hud
import verify_battle_interface as interface
import verify_ui_binding as binding
import verify_ui_second_update as tool
import verify_ui_window as window
from test_verify_ui_first_update import fixture as first_fixture

def native_window(row, slot):
    original = bytes.fromhex(row['controller_windows'][slot]['raw']['raw_hex'])
    values = list(struct.unpack('<%dI' % (len(original) // 4), original))
    embed = row['embedded_streams'][slot]
    values[0x2698 // 4] = None
    cursor = embed['cursor']
    for i in range(1, cursor, 2):
        values[0x26dc // 4 + i] = None
    for offset in window.EXCLUDED_OFFSETS:
        values[offset // 4] = None
    commands, _ = tool._expected_commands(row, slot)
    return {'slot': slot, 'scalar_words': values, 'commands': commands,
            'glyph_positions_hex': embed['glyph_positions']['raw_hex'],
            'glyph_metrics_hex': embed['glyph_metrics']['raw_hex']}

def native_hud(row):
    source = hud._original_widgets(row)
    groups = []
    for group in source:
        output = []
        for item in group:
            words = list(item['words']); words[16] = 0
            output.append({'words': words,
                           'runtime': None if item['runtime'] is None else list(item['runtime'][:10]),
                           'text_hex': None if item['text'] is None else item['text'].hex()})
        groups.append(output)
    return {'schema': hud.NATIVE_SCHEMA,
            'scope': "RUW and HUD state at an isolated dispatcher boundary",
            'execution': 'ui-update', 'game_mode': 3, 'selected': 0,
            'vars_hex': bytes(hud.VARS_BYTES).hex(),
            'globals': {key: row['shared_globals'][key] for key in hud.GLOBAL_KEYS},
            'groups': groups}

def native_snapshot(row):
    manager = bytes.fromhex(row['manager_raw']['raw_hex'])
    words = list(struct.unpack('<%dI' % (len(manager) // 4), manager))
    for offset in tool.MANAGER_NORMALIZED:
        if offset in (0, 4, 8, 0xc, 0x10, 0x14, 0x18): words[offset // 4] = 0
        elif offset in (0x44, 0x124): words[offset // 4] = 1
        elif offset in (0x94, 0x9c, 0x114): words[offset // 4] = 1
        elif offset in (0x174, 0x17c): words[offset // 4] = 2
        elif offset == 0x1f4: words[offset // 4] = 1
    cache = bytes.fromhex(row['party_selection_counts_and_table']['raw_hex'])
    counts = list(struct.unpack_from('<8I', cache))
    flat_rows = struct.unpack_from('<64i', cache, 32)
    accum = struct.unpack('<16I', bytes.fromhex(row['type2_widget_index_accumulators']['raw_hex']))
    state = bytes.fromhex(row['battle_ui_party_refresh_state']['raw_hex'])
    return {'manager_words': words,
            'windows': [native_window(row, i) for i in range(2)],
            'hud': native_hud(row),
            'party_counts': counts,
            'party_rows': [list(flat_rows[i * 8:(i + 1) * 8]) for i in range(8)],
            'resource_accumulators': list(accum),
            'command_changed': [0] * 8, 'command_values': [0] * 8, 'command': 0,
            'selected_alpha': struct.unpack_from('<I', state, 8)[0],
            'selected_alpha_step': -4, 'secondary_pulse': 0, 'secondary_pulse_step': 1,
            'cost_pulse': 0, 'hp_cost_gate': 0, 'type3_blink': 0,
            'type3_formatted_hex': bytes(256).hex(), 'type3_padded_name_hex': bytes(256).hex(),
            'type3_current_number_hex': bytes(64).hex(), 'type3_maximum_number_hex': bytes(64).hex(),
            'formation_count': 0, 'formation_unit_ids': [0] * 8,
            'formation_counts': [0] * 8, 'formation_groups': [0] * 8}

def fixture():
    raw_reference, _ = first_fixture()
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / 'source.jsonl'
        source.write_text(''.join(json.dumps(row) + '\n' for row in raw_reference))
        reference = tool.load_reference(source)
    rows = reference[1]
    for row in rows:
        row['battle_ui_party_refresh_state'] = {
            'address': 0x1fd770, 'bytes': 32,
            'raw_hex': (struct.pack('<I', row['shared_globals'].get('ui_selection_pointer', 0)) +
                        bytes(4) + struct.pack('<I', 128) + bytes(20)).hex(),
        }
    native = {'schema': tool.NATIVE_SCHEMA, 'scope': tool.NATIVE_SCOPE,
              'snapshots': {'before': native_snapshot(rows[2]),
                            'after': native_snapshot(rows[3])},
              'frame': 105, 'input_hashes': {'base': 'a' * 64}}
    return reference, native

class VerifyUiSecondUpdateTests(unittest.TestCase):
    def setUp(self): self.reference, self.native = fixture()

    def test_exact_second_callback_pair_and_scoped_counts(self):
        report = tool.compare(self.reference, self.native)
        self.assertEqual(report['status'], 'pass')
        self.assertEqual(report['native_schema'], tool.NATIVE_SCHEMA)
        self.assertEqual(report['scope'], tool.NATIVE_SCOPE)
        self.assertEqual(report['execution'], 'isolated')
        self.assertEqual(report['snapshots']['before']['native_report'], {
            'schema': tool.NATIVE_SCHEMA, 'scope': tool.NATIVE_SCOPE, 'execution': 'isolated'})
        self.assertEqual(report['reference_second_callback_frames'],
                         [self.reference[1][2]['frame'], self.reference[1][3]['frame']])
        self.assertEqual(report['compared_party_cache_words'], 144)
        self.assertEqual(report['compared_accumulator_words'], 32)
        self.assertGreater(report['compared_window_words'], 20000)
        self.assertGreater(report['compared_glyph_bytes'], 0)
        self.assertEqual(report['compared_hud']['after']['reference_schema'], self.reference[0]['schema'])
        self.assertEqual(report['compared_hud']['after']['native_execution'], 'ui-update')

    def test_scheduled_report_schema_and_nested_execution_are_preserved(self):
        native = copy.deepcopy(self.native)
        native['schema'] = tool.SCHEDULED_SCHEMA
        native['scope'] = tool.SCHEDULED_SCOPE
        for phase in ('before', 'after'):
            native['snapshots'][phase]['hud']['scope'] = 'RUW and HUD state at a scheduled dispatcher boundary'
            native['snapshots'][phase]['hud']['execution'] = 'scheduled-ui-update'
        report = tool.compare(self.reference, native)
        self.assertEqual(report['status'], 'pass')
        self.assertEqual(report['native_schema'], tool.SCHEDULED_SCHEMA)
        self.assertEqual(report['scope'], tool.SCHEDULED_SCOPE)
        self.assertEqual(report['execution'], 'scheduled')
        self.assertIn('no whole-scheduler equivalence claim', report['comparison_scope'])
        for phase in ('before', 'after'):
            self.assertEqual(report['snapshots'][phase]['native_report'], {
                'schema': tool.SCHEDULED_SCHEMA, 'scope': tool.SCHEDULED_SCOPE,
                'execution': 'scheduled'})
            self.assertEqual(report['compared_hud'][phase]['scope'],
                             'RUW and HUD state at a scheduled dispatcher boundary')
            self.assertEqual(report['compared_hud'][phase]['native_execution'],
                             'scheduled-ui-update')
        self.assertTrue(any('after the named native callbacks only' in value
                            for value in report['unsupported']))

    def test_isolated_and_scheduled_schema_scope_mixups_fail(self):
        cases = (
            (tool.SCHEDULED_SCHEMA, tool.NATIVE_SCOPE),
            (tool.NATIVE_SCHEMA, tool.SCHEDULED_SCOPE),
            ('okage-native-ui-other-v1', tool.NATIVE_SCOPE),
            ([], tool.NATIVE_SCOPE),
        )
        for schema, scope in cases:
            with self.subTest(schema=schema, scope=scope):
                native = copy.deepcopy(self.native)
                native['schema'], native['scope'] = schema, scope
                with self.assertRaises(tool.Invalid):
                    tool.compare(self.reference, native)
        native = copy.deepcopy(self.native)
        native['schema'], native['scope'] = tool.SCHEDULED_SCHEMA, tool.SCHEDULED_SCOPE
        native['snapshots']['after']['hud']['execution'] = 'ui-update'
        with self.assertRaises(tool.Invalid):
            tool.compare(self.reference, native)

    def test_native_window_hud_cache_and_global_corruptions_are_retained(self):
        cases = (
            ('window transform', lambda n: n['snapshots']['after']['windows'][0]['scalar_words'].__setitem__(0x270 // 4, 99)),
            ('window opacity', lambda n: n['snapshots']['after']['windows'][0]['scalar_words'].__setitem__(0x2f0 // 4, 99)),
            ('glyph', lambda n: n['snapshots']['after']['windows'][0].__setitem__('glyph_positions_hex', 'ff' + n['snapshots']['after']['windows'][0]['glyph_positions_hex'][2:])),
            ('HUD', lambda n: n['snapshots']['after']['hud']['groups'][2][0]['runtime'].__setitem__(0, 77)),
            ('party cache', lambda n: n['snapshots']['after']['party_rows'][0].__setitem__(0, 77)),
            ('accumulator', lambda n: n['snapshots']['after']['resource_accumulators'].__setitem__(0, 77)),
            ('selected alpha', lambda n: n['snapshots']['after'].__setitem__('selected_alpha', 77)),
        )
        for label, mutate in cases:
            with self.subTest(label=label):
                native = copy.deepcopy(self.native); mutate(native)
                report = tool.compare(self.reference, native)
                self.assertEqual(report['status'], 'fail')
                self.assertTrue(any(label.split()[0].lower() in str(d).lower() or
                                    label.lower() in str(d).lower()
                                    for d in report['differences']), report['differences'][:2])

    def test_four_original_raw_ruw_word_differences_are_preserved(self):
        row = self.reference[1][3]
        raw = bytearray.fromhex(row['ruw_slot0']['raw_owner']['raw_hex'])
        for group, index, value in ((2, 45, 31), (2, 46, 31), (2, 47, 31), (5, 9, 29)):
            at = hud.GROUPS_OFFSET + (hud.GROUP_RECORD_OFFSETS[group] + index) * hud.RECORD_BYTES + 6 * 4
            struct.pack_into('<I', raw, at, value)
            self.native['snapshots']['after']['hud']['groups'][group][index]['words'][6] = 0
        row['ruw_slot0']['raw_owner']['raw_hex'] = raw.hex()
        report = tool.compare(self.reference, self.native)
        self.assertEqual(report['status'], 'fail')
        actual = [(d['field'], d['expected'], d['actual']) for d in report['differences']]
        self.assertEqual(actual, [
            ('groups[2][45].words[6]', 31, 0),
            ('groups[2][46].words[6]', 31, 0),
            ('groups[2][47].words[6]', 31, 0),
            ('groups[5][9].words[6]', 29, 0),
        ])

    def test_malformed_and_unobserved_native_fields_fail_closed(self):
        native = copy.deepcopy(self.native); del native['snapshots']['after']['hud']
        with self.assertRaises(tool.Invalid): tool.compare(self.reference, native)
        native = copy.deepcopy(self.native); native['snapshots']['before']['command_changed'][0] = True
        with self.assertRaises(tool.Invalid): tool.compare(self.reference, native)
        native = copy.deepcopy(self.native); native['snapshots']['after']['type3_formatted_hex'] = 'xyz'
        with self.assertRaises(tool.Invalid): tool.compare(self.reference, native)
        native = copy.deepcopy(self.native); native['snapshots']['after']['manager_words'][0] = 0x11fec8
        report = tool.compare(self.reference, native)
        self.assertEqual(report['status'], 'fail')

    def test_malformed_source_manager_and_owner_links_are_rejected(self):
        def corrupt(row, key, offset, value):
            raw = bytearray.fromhex(row[key]['raw_hex'])
            struct.pack_into('<I', raw, offset, value)
            row[key]['raw_hex'] = raw.hex()

        mutations = (
            ('manager next sibling', 'manager_raw', 0x10, 0x1234),
            ('manager child', 'manager_raw', 0x18, 0x1234),
            ('root manager backlink', 'battle_root', 0x8060, 0x1234),
        )
        for label, key, offset, value in mutations:
            with self.subTest(label=label):
                reference = copy.deepcopy(self.reference)
                row = reference[1][2]
                corrupt(row, key, offset, value)
                with self.assertRaises(tool.Invalid):
                    tool.compare(reference, copy.deepcopy(self.native))
        reference = copy.deepcopy(self.reference)
        row = reference[1][2]
        raw = bytearray.fromhex(row['battle_root']['raw_hex'])
        first_child = struct.unpack_from('<I', raw, 0x8064)[0]
        corrupt(row, 'battle_root', 0x8068, first_child)
        with self.assertRaises(tool.Invalid):
            tool.compare(reference, copy.deepcopy(self.native))

    def test_cli_writes_failure_report_and_refuses_clobber(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); ref = root / 'ref.jsonl'; nat = root / 'native.json'; out = root / 'report.json'
            ref.write_text('\n'.join(json.dumps(row) for row in
                                    [self.reference[0], *self.reference[1], self.reference[2]]) + '\n')
            native = copy.deepcopy(self.native); native['snapshots']['after']['party_counts'][0] ^= 1
            nat.write_text(json.dumps(native))
            command = [sys.executable, str(Path(tool.__file__)), '--reference', str(ref),
                       '--native', str(nat), '--output', str(out)]
            proc = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 1, proc.stderr + proc.stdout)
            self.assertEqual(json.loads(out.read_text())['status'], 'fail')
            proc = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 2)

if __name__ == '__main__': unittest.main()
