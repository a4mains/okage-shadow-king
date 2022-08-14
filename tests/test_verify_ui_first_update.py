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
import verify_ui_first_update as tool
import verify_ui_binding as binding
import verify_ui_window as window
from test_verify_ui_binding import fixture as binding_fixture

def raw_wrapper(address, data):
    return {'address': address, 'bytes': len(data), 'raw_hex': bytes(data).hex()}

def put(data, offset, value):
    struct.pack_into('<I', data, offset, value)

def get(data, offset):
    return struct.unpack_from('<I', data, offset)[0]

def fixture():
    reference, native = binding_fixture()
    root = reference[1]['root']; manager = reference[1]['manager']
    enemy, stage, camera, party = 0x310000, 0x320000, 0x330000, 0x340000
    for row in reference[1:-1]:
        row['shared_globals']['battle_ui_texture'] = 81
        row['pad_globals'] = {'0x33958c': 0, '0x339590': 0,
                              '0x1fcb60': 1, '0x1fcb64': 0, '0x1fcb68': 0}
        row['type2_widget_index_accumulators'] = raw_wrapper(0x200a88, bytes(64))
        root_bytes = bytearray(0x81d0)
        for offset, value in ((0x18, stage), (0x8060, manager), (0x8064, stage),
                              (0x8068, camera), (0x806c, party), (0x8070, enemy)):
            put(root_bytes, offset, value)
        row['battle_root'] = raw_wrapper(root, root_bytes)

    source = bytearray(0xef0)
    for off, val in ((0, 0x11fec8), (4, 0x120ad0), (8, 0x120b48),
                     (0xc, enemy), (0x14, root), (0x44, 81), (0x124, 81),
                     (0x94, 0x1fd820), (0x9c, 0x164090),
                     (0x174, 0x1fdb88), (0x17c, 0x1684d0),
                     (0xe40, 2), (0xe90, 0), (0xe94, 1)):
        put(source, off, val)
    for i, (stream, _a, _b, typ, callback) in enumerate(window.STREAMS):
        base = 0x40 + i * 0xe0
        for off, val in ((base, i), (base + 0x50, 0x50), (base + 0x54, stream),
                         (base + 0x5c, callback), (base + 0x60, typ)):
            put(source, off, val)
    before = bytes(source)
    after = bytearray(before)
    for off, val in ((0x114, manager), (0x1f4, manager), (0xe48, 1),
                     (0xe4c, 0xffffff9c), (0xe50, 1), (0xe8c, 2),
                     (0xa4, 19), (0x184, 0)):
        put(after, off, val)
    # A simple finite type3 angle gives the source add constant without wrap.
    put(after, 0x144, 0x3d567451)

    for i, row in enumerate(reference[1:-1]):
        data = before if i == 0 else after
        row['manager_raw'] = raw_wrapper(manager, data)
        for layer in range(2):
            base = 0x40 + layer * 0xe0
            row[f'layer{layer}'] = raw_wrapper(manager + base, data[base:base + 0xe0])

    native['schema'] = tool.NATIVE_SCHEMA
    native['scope'] = tool.NATIVE_SCOPE
    native['manager_before'] = list(struct.unpack('<%dI' % (len(before) // 4), before))
    native['manager_after'] = list(struct.unpack('<%dI' % (len(after) // 4), after))
    for phase, words in [('manager_before', native['manager_before']),
                         ('manager_after', native['manager_after'])]:
        # The native owner uses logical host IDs in place of source pointers.
        for off in (0, 4, 8, 0xc, 0x10, 0x14, 0x18): words[off // 4] = 0
        words[0x44 // 4] = words[0x124 // 4] = 1
        words[0x94 // 4] = words[0x9c // 4] = 1
        words[0x174 // 4] = words[0x17c // 4] = 2
        if phase == 'manager_before':
            words[0x114 // 4] = words[0x1f4 // 4] = 0
        else:
            words[0x114 // 4] = words[0x1f4 // 4] = 1

    pre = reference[1]
    native['windows_before'] = []
    for slot in range(2):
        wrapper = pre['controller_windows'][slot]['raw']
        data = binding.raw(wrapper, 'window', window.WINDOW_BYTES,
                           pre['controller_windows'][slot]['pointer'])
        values = list(struct.unpack('<%dI' % window.WINDOW_WORDS, data))
        for idx in window.EXCLUDED_WORDS: values[idx] = None
        native['windows_before'].append(values)
    native['resource_accumulators'] = [0] * 16
    native['input'] = dict(tool.NEUTRAL_INPUT)
    return reference, native

class VerifyUiFirstUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.reference, self.native = fixture()

    def compare(self):
        path = self.path / 'original.jsonl'
        path.write_text(''.join(json.dumps(row) + '\n' for row in self.reference))
        return tool.compare(tool.load_reference(path), self.native)

    def test_complete_first_update_boundary(self):
        report = self.compare()
        self.assertEqual(report['status'], 'pass')
        self.assertEqual(report['windows_before']['compared_words'], 14146)
        self.assertEqual(report['manager']['compared_words'], 2 * (tool.MANAGER_WORDS - 15))
        self.assertEqual(report['resource_accumulators']['compared_words'], 16)
        self.assertEqual(report['binding']['compared_glyph_bytes'], 22528)

    def test_native_window_before_and_after_manager_mismatches_fail(self):
        self.native['windows_before'][0][0x2a0 // 4] ^= 1
        self.native['manager_after'][0xe8c // 4] ^= 1
        report = self.compare()
        self.assertEqual(report['status'], 'fail')
        self.assertGreaterEqual(report['difference_count'], 2)
        self.assertEqual(report['first_difference']['component'], 'manager')

    def test_original_parent_callback_texture_or_backlink_corruption_rejected(self):
        for index in (0, 1):
            ref, native = fixture()
            data = bytearray.fromhex(ref[1 + index]['manager_raw']['raw_hex'])
            put(data, 0x14, 0 if index == 0 else 0x1234)
            ref[1 + index]['manager_raw'] = raw_wrapper(ref[1 + index]['manager'], data)
            self.reference, self.native = ref, native
            with self.assertRaises(tool.Invalid): self.compare()
        self.reference, self.native = fixture()
        data = bytearray.fromhex(self.reference[1]['manager_raw']['raw_hex']); put(data, 0, 0)
        self.reference[1]['manager_raw'] = raw_wrapper(self.reference[1]['manager'], data)
        with self.assertRaises(tool.Invalid): self.compare()

    def test_accumulator_input_and_unknown_mask_are_strict(self):
        self.native['resource_accumulators'][4] = 1
        self.assertEqual(self.compare()['status'], 'fail')
        self.reference, self.native = fixture()
        self.native['input']['held'] = True
        with self.assertRaises(tool.Invalid): self.compare()
        self.reference, self.native = fixture()
        self.native['windows_before'][0][0] = 0
        with self.assertRaises(tool.Invalid): self.compare()

    def test_manager_extent_and_binding_schema_reject_malformed_inputs(self):
        self.native['manager_after'].pop()
        with self.assertRaises(tool.Invalid): self.compare()
        self.reference, self.native = fixture()
        self.native['hud']['groups'][0].pop()
        with self.assertRaises(tool.Invalid): self.compare()

    def test_cli_refuses_clobber_and_duplicate_json_keys(self):
        ref = self.path / 'reference.jsonl'
        ref.write_text(''.join(json.dumps(row) + '\n' for row in self.reference))
        native = self.path / 'native.json'; native.write_text(json.dumps(self.native))
        output = self.path / 'report.json'
        args = ['--reference', str(ref), '--native', str(native), '--output', str(output)]
        self.assertEqual(tool.main(args), 0)
        first = output.read_bytes()
        self.assertEqual(tool.main(args), 2)
        self.assertEqual(output.read_bytes(), first)
        native.write_text('{"schema":1,"schema":2}')
        args[args.index(str(native))] = str(native)
        args[args.index(str(output))] = str(self.path / 'bad.json')
        self.assertEqual(tool.main(args), 2)
        self.assertFalse((self.path / 'bad.json').exists())

if __name__ == '__main__': unittest.main()
