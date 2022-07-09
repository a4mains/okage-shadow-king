import copy
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import verify_battle_interface as tool
from test_verify_ui_window import make_reference, raw_wrapper, write_jsonl

TOOL = Path(tool.__file__)

def replace_word(wrapper, offset, value):
    data = bytearray.fromhex(wrapper['raw_hex'])
    struct.pack_into('<I', data, offset, value)
    wrapper['raw_hex'] = data.hex()

def fixture():
    reference, windows = make_reference()
    entry, complete = reference[1:3]
    root, manager = entry['root'], complete['manager']
    children = (0x1dc52a0, 0x1dc52f0, 0x1dc5450, 0x1dc5c70)
    body = bytearray(0x81d0)
    struct.pack_into('<I', body, 0x18, children[0])
    struct.pack_into('<4I', body, 0x8064, *children)
    for row in (entry, complete):
        row['battle_root'] = raw_wrapper(root, body)
        row['shared_globals'] = {'battle_root_pointer': root, 'battle_ui_texture': 81}
    original = bytearray.fromhex(complete['manager_raw']['raw_hex'])
    struct.pack_into('<7I', original, 0, *tool.CALLBACKS, children[3], 0, root, 0)
    for i, (stream, _a, _b, _t, _cb) in enumerate(tool.window.STREAMS):
        base = 0x40 + i * 0xe0
        struct.pack_into('<I', original, base + 4, 81)
        struct.pack_into('<I', original, base + 0x50, 0x50)
        complete[f'layer{i}'] = raw_wrapper(manager + base, original[base:base + 0xe0])
        for offset, value in ((8, 81), (0x2698, stream), (0x3bf0, stream)):
            replace_word(complete['controller_windows'][i]['raw'], offset, value)
    struct.pack_into('<I', original, 0xe44, 3)
    complete['manager_raw'] = raw_wrapper(manager, original)
    complete['controller_stream_state'] = raw_wrapper(0x1f9ca0, struct.pack('<II', 0x1fdb88, 4))
    values = list(struct.unpack('<956I', original))
    for offset in tool.NORMALIZED_OFFSETS:
        values[offset // 4] = 0
    for i in range(2):
        base = 0x40 + i * 0xe0
        values[(base + 4) // 4] = 1
        values[(base + 0x54) // 4] = i + 1
        values[(base + 0x5c) // 4] = i + 1
    native = {'schema': tool.NATIVE_SCHEMA, 'frame': 104451, 'scope': tool.SCOPE,
              'manager_words': values, 'current_stream': 2, 'stream_cursor': 4,
              'atlas_dirty': False, 'windows': windows['windows']}
    return reference, native

class VerifyBattleInterfaceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.reference_path = self.directory / 'original.jsonl'
        self.native_path = self.directory / 'native.json'
        self.output = self.directory / 'report.json'
        self.reference, self.native = fixture()

    def save(self):
        write_jsonl(self.reference_path, self.reference)
        self.native_path.write_text(json.dumps(self.native))

    def compare(self):
        self.save()
        return tool.compare(tool.load_reference(self.reference_path), self.native)

    def cli(self):
        return subprocess.run([sys.executable, str(TOOL), '--reference', str(self.reference_path),
                               '--native', str(self.native_path), '--output', str(self.output)],
                              text=True, capture_output=True)

    def manager_word(self, offset, value):
        complete = self.reference[2]
        replace_word(complete['manager_raw'], offset, value)
        data = bytes.fromhex(complete['manager_raw']['raw_hex'])
        for i in range(2):
            base = 0x40 + i * 0xe0
            complete[f'layer{i}'] = raw_wrapper(complete['manager'] + base, data[base:base + 0xe0])

    def test_complete_exact_scope_hashes_and_independent_frames(self):
        self.save()
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(self.output.read_text())
        self.assertEqual(report['status'], 'pass')
        self.assertEqual(report['compared_words'], 15089)
        self.assertEqual(report['manager']['compared_words'], 943)
        self.assertEqual(report['manager']['normalized_words'], 13)
        self.assertEqual(report['windows']['compared_words'], 14146)
        self.assertEqual(report['builder']['compared_fields'], 2)
        self.assertEqual((report['native_frame'], report['reference_frame']), (104451, 126))
        self.assertEqual(report['atlas_dirty'], {'native': False, 'original': None, 'compared': False})
        self.assertEqual(report['inputs']['reference']['sha256'], hashlib.sha256(self.reference_path.read_bytes()).hexdigest())
        self.assertTrue(any('stream contents are not emitted' in s for s in report['unsupported']))

    def test_manager_window_and_builder_differences_retained(self):
        self.native['manager_words'][0xe44 // 4] = 4  # Semantic scalar, not pointer.
        self.native['manager_words'][0x114 // 4] = 7  # Layer0 d4: pre-activation zero.
        self.native['windows'][1]['words'][400] ^= 1
        self.native['stream_cursor'] = 5
        self.save()
        result = self.cli()
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(self.output.read_text())
        self.assertEqual(report['difference_count'], 4)
        self.assertEqual(report['first_difference']['offset'], 0x114)
        self.assertEqual({d['component'] for d in report['differences']}, {'manager', 'window', 'builder'})

    def test_each_normalized_word_has_an_enforced_native_binding(self):
        for offset in tool.NORMALIZED_OFFSETS:
            with self.subTest(offset=offset):
                self.reference, self.native = fixture()
                self.native['manager_words'][offset // 4] = 0x123456
                with self.assertRaises(tool.Invalid):
                    self.compare()

    def test_original_callbacks_links_and_texture_are_not_arbitrary_exclusions(self):
        for offset in tool.NORMALIZED_OFFSETS:
            with self.subTest(offset=offset):
                self.reference, self.native = fixture()
                self.manager_word(offset, 0x123456)
                with self.assertRaises(tool.Invalid):
                    self.compare()
        self.reference, self.native = fixture()
        replace_word(self.reference[2]['controller_windows'][0]['raw'], 8, 82)
        with self.assertRaises(tool.Invalid):
            self.compare()

    def test_root_owner_and_raw_address_evidence_required(self):
        for field in ('battle_root', 'controller_stream_state', 'manager_raw', 'layer0'):
            with self.subTest(field=field):
                self.reference, self.native = fixture()
                self.reference[2][field]['address'] += 4
                with self.assertRaises(tool.Invalid):
                    self.compare()
        self.reference, self.native = fixture()
        replace_word(self.reference[1]['battle_root'], 0x8070, 123)
        with self.assertRaises(tool.Invalid):
            self.compare()

    def test_parent_flag_inheritance_is_checked_without_ignoring_native_flags(self):
        for row in self.reference[1:3]:
            replace_word(row['battle_root'], 0x1c, 0x98000001)
        self.manager_word(0x1c, 0x18000000)
        self.native['manager_words'][7] = 0x18000000
        self.assertEqual(self.compare()['status'], 'pass')
        self.native['manager_words'][7] = 0
        self.assertEqual(self.compare()['difference_count'], 1)
        self.manager_word(0x1c, 0x98000001)
        with self.assertRaises(tool.Invalid):
            self.compare()

    def test_exact_schema_extents_types_and_window_mask(self):
        mutations = [lambda n: n['manager_words'].pop(), lambda n: n['manager_words'].append(0),
                     lambda n: n.update(extra=1), lambda n: n.pop('current_stream'),
                     lambda n: n.update(frame=True), lambda n: n.update(atlas_dirty=0),
                     lambda n: n.update(stream_cursor=False),
                     lambda n: n['manager_words'].__setitem__(200, False),
                     lambda n: n['manager_words'].__setitem__(200, None),
                     lambda n: n['manager_words'].__setitem__(200, -1),
                     lambda n: n['manager_words'].__setitem__(200, 1 << 32),
                     lambda n: n['windows'][0]['words'].__setitem__(0, 0),
                     lambda n: n['windows'][0]['words'].__setitem__(100, None),
                     lambda n: n['windows'].reverse()]
        for mutate in mutations:
            self.reference, self.native = fixture()
            mutate(self.native)
            with self.assertRaises(tool.Invalid):
                self.compare()

    def test_missing_duplicate_snapshots_and_unimplemented_activation_rejected(self):
        for index in (1, 2):
            self.reference, self.native = fixture()
            self.reference.pop(index)
            with self.assertRaises(tool.Invalid):
                self.compare()
        self.reference, self.native = fixture()
        self.reference.insert(2, copy.deepcopy(self.reference[1]))
        with self.assertRaises(tool.Invalid):
            self.compare()
        self.reference, self.native = fixture()
        self.manager_word(0x114, self.reference[2]['manager'])
        with self.assertRaises(tool.Invalid):
            self.compare()

    def test_builder_and_authored_constructor_identity(self):
        replace_word(self.reference[2]['controller_stream_state'], 0, 0x1fd820)
        with self.assertRaises(tool.Invalid):
            self.compare()
        self.reference, self.native = fixture()
        replace_word(self.reference[2]['authored_interface_records_and_terminator'], 0x58, 0x168228)
        with self.assertRaises(tool.Invalid):
            self.compare()
        self.reference, self.native = fixture()
        self.native['current_stream'] = 1
        self.assertEqual(self.compare()['first_difference']['field'], 'current_stream')

    def test_dirty_is_not_invented_original_comparison(self):
        self.native['atlas_dirty'] = True
        report = self.compare()
        self.assertEqual(report['status'], 'pass')
        self.assertFalse(report['atlas_dirty']['compared'])

    def test_outputs_never_overwrite_and_json_duplicate_keys_rejected(self):
        self.save()
        self.output.write_text('retained')
        self.assertEqual(self.cli().returncode, 2)
        self.assertEqual(self.output.read_text(), 'retained')
        self.output.unlink()
        self.output.symlink_to(self.native_path)
        self.assertEqual(self.cli().returncode, 2)
        self.output.unlink()
        self.native_path.write_text(self.native_path.read_text()[:-1] + ',"frame":123}')
        self.assertEqual(self.cli().returncode, 2)
        self.assertFalse(self.output.exists())
        self.save()
        text = self.reference_path.read_text().splitlines()
        text[1] = text[1][:-1] + ',"frame":123}'
        self.reference_path.write_text('\n'.join(text) + '\n')
        self.assertEqual(self.cli().returncode, 2)
        self.assertFalse(self.output.exists())

if __name__ == '__main__':
    unittest.main()
