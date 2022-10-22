import copy
import importlib.util
from pathlib import Path
import struct
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools' / 'verify_battle_callbacks.py'
SPEC = importlib.util.spec_from_file_location('battle_callbacks', SCRIPT)
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)

def wrapped(data, address=0):
    return {'address': address, 'bytes': len(data), 'raw_hex': data.hex()}

def fixture():
    root = bytearray(0x81d0)
    struct.pack_into('<I', root, 0x24, 1)
    struct.pack_into('<I', root, 0x8068, 0x300000)
    camera = bytes(0x150)
    events = []
    actors = []

    def pair(kind, owner, extra=None):
        for edge in ('entry', 'return'):
            row = {'event': 'parent_phase0_to1_return' if kind == 'parent' and edge == 'return' else f'{kind}_{edge}', 'sequence': len(events), 'root': 0x200000,
                   'owner': owner, 'sp': 0x700000, 'ra': 0x114ff0,
                   'pc': 0x114ff0 if edge == 'return' else M.ENTRY_PC[kind],
                   'post': edge == 'return', 'frame': 126,
                   'root_header': wrapped(root, 0x200000), 'camera_state': wrapped(camera, 0x300000), 'camera_owner': 0x300000}
            if kind == 'parent' and edge == 'entry':
                before = bytearray(root)
                struct.pack_into('<I', before, 0x24, 0)
                row['root_header'] = wrapped(before, 0x200000)
            row.update(copy.deepcopy(extra or {}))
            events.append(row)

    pair('parent', 0x200000)
    pair('camera', 0x300000)
    for enemy, entity in ((False, 0), (False, 3), (True, 4), (True, 5), (True, 6)):
        task = bytearray(0x3e0 if enemy else 0x3d0)
        struct.pack_into('<H', task, 0x130, entity)
        record = bytes(0x6c if enemy else 0x50)
        extra = {'actor_owner': wrapped(task, 0x400000 + entity * 0x1000), 'private_stats' if enemy else 'live_stats': wrapped(record),
                 'model': {'channel_count': 4, 'channels': wrapped(bytes(0x90 * 4))}}
        kind = 'enemy' if enemy else 'party'
        pair(kind + '_primary', 0x400000 + entity * 0x1000, extra)
        pair(kind + '_secondary', 0x400000 + entity * 0x1000, extra)
        for boundary in ('before', 'after-first', 'after-second'):
            actors.append({'enemy': enemy, 'entity_id': entity, 'boundary': boundary,
                           'task_hex': task.hex(), 'record_hex': record.hex(), 'channels': [0] * 16})
    pair('ui_second_layer', 0x500000)
    rows = [{'schema': 'okage-battle-callbacks-v1', 'source_elf_sha256': M.ELF}, *events,
            {'event': 'finish', 'rows': 26, 'ui_completed': True}]
    native = {'schema': 'okage-native-battle-callbacks-v1', 'scope': M.NATIVE_SCOPE,
              'complete': True, 'error': '', 'frame': 104452, 'actors': actors,
              'camera': [0] * 56, 'root_hex': root.hex(),
              'events': ['parent.activation.entry', 'parent.activation.return', 'stage.first.return',
                         'stage.second.return', 'camera.first.return', 'camera.second.return', 'party.parent.return',
                         'party.child.0.return', 'party.child.3.return', 'enemy.parent.return',
                         'enemy.child.4.return', 'enemy.child.5.return', 'enemy.child.6.return',
                         'ui.first.return', 'ui.second.return']}
    return rows, native

class CallbackComparisonTests(unittest.TestCase):
    def test_matching_synthetic_boundaries(self):
        result = M.compare(*fixture())
        self.assertEqual(result['status'], 'pass')
        self.assertEqual(result['counts']['channel_words'], 320)
        self.assertEqual(result['counts']['camera_words'], 112)
        self.assertTrue(result['unsupported'])

    def test_charge_difference_is_retained(self):
        rows, native = fixture()
        record = bytearray.fromhex(native['actors'][2]['record_hex'])
        struct.pack_into('<h', record, 0x26, 56)
        native['actors'][2]['record_hex'] = record.hex()
        result = M.compare(rows, native)
        self.assertEqual(result['status'], 'fail')
        self.assertEqual(result['first_difference']['path'], 'actor[0].party_secondary_return.record[0:50]')

    def test_channel_word_is_exact(self):
        rows, native = fixture()
        native['actors'][0]['channels'][7] = 1
        result = M.compare(rows, native)
        self.assertEqual(result['first_difference']['path'], 'actor[0].party_primary_entry.channel[1][3]')

    def test_camera_sequence_phase_has_correct_source_offset(self):
        rows, native = fixture()
        for index in (4, 26):
            data = bytearray(0x150)
            struct.pack_into('<I', data, 0x140, 3)
            rows[index]['camera_state'] = wrapped(data, 0x300000)
        native['camera'][55] = 3
        self.assertEqual(M.compare(rows, native)['status'], 'pass')

    def test_missing_finish_is_rejected(self):
        rows, native = fixture()
        with self.assertRaisesRegex(ValueError, 'expected header'):
            M.compare(rows[:-1], native)

    def test_misordered_callback_is_rejected(self):
        rows, native = fixture()
        rows[5], rows[7] = rows[7], rows[5]
        with self.assertRaisesRegex(ValueError, 'callback order'):
            M.compare(rows, native)

    def test_owner_change_between_callbacks_is_rejected(self):
        rows, native = fixture()
        rows[7]['owner'] += 4
        rows[8]['owner'] += 4
        with self.assertRaisesRegex(ValueError, 'primary and secondary'):
            M.compare(rows, native)

    def test_unknown_native_boundary_is_rejected(self):
        rows, native = fixture()
        native['actors'][0]['boundary'] = 'after'
        with self.assertRaisesRegex(ValueError, 'native boundary'):
            M.compare(rows, native)

    def test_missing_pair_metadata_rejected(self):
        rows, native = fixture()
        for row in rows[3:5]:
            for key in ('owner', 'ra', 'pc', 'sp'):
                row.pop(key)
        with self.assertRaisesRegex(ValueError, 'callback address'):
            M.compare(rows, native)

    def test_wrong_entry_pc_rejected(self):
        rows, native = fixture()
        rows[3]['pc'] += 4
        with self.assertRaisesRegex(ValueError, 'entry PC'):
            M.compare(rows, native)

    def test_parent_without_phase_transition_rejected(self):
        rows, native = fixture()
        rows[2]['root_header'] = copy.deepcopy(rows[1]['root_header'])
        with self.assertRaisesRegex(ValueError, 'phase transition'):
            M.compare(rows, native)

    def test_native_event_order_rejected(self):
        rows, native = fixture()
        native['events'][7:9] = reversed(native['events'][7:9])
        with self.assertRaisesRegex(ValueError, 'event order'):
            M.compare(rows, native)

    def test_parent_entry_can_have_unconstructed_camera(self):
        rows, native = fixture()
        rows[1]['camera_state'] = None
        rows[1].pop('camera_owner')
        self.assertEqual(M.compare(rows, native)['status'], 'pass')

    def test_enemy_string_pointers_are_explicitly_uncompared(self):
        rows, native = fixture()
        row = native['actors'][6]
        b = bytearray.fromhex(row['record_hex'])
        struct.pack_into('<2I', b, 0, 123, 456)
        row['record_hex'] = b.hex()
        result = M.compare(rows, native)
        self.assertEqual(result['status'], 'pass')
        self.assertTrue(any('string pointers' in x for x in result['unsupported']))

    def test_native_guard_failure_is_not_a_match(self):
        rows, native = fixture()
        native['complete'] = False
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            M.compare(rows, native)

if __name__ == '__main__':
    unittest.main()
