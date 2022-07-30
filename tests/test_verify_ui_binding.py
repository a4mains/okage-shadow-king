import copy
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import verify_ui_binding as tool
from test_verify_battle_hud import fixture as hud_fixture
from test_verify_ui_window import make_reference, raw_wrapper

def put(wrapper, offset, value):
    data = bytearray.fromhex(wrapper['raw_hex'])
    struct.pack_into('<I', data, offset, value)
    wrapper['raw_hex'] = data.hex()

def fixture():
    old, _ = make_reference()
    hud_ref, native_hud = hud_fixture()
    base = copy.deepcopy(old[2])
    base['ruw_slot0'] = hud_ref[2]['ruw_slot0']
    base['shared_globals'] = hud_ref[2]['shared_globals']
    base['embedded_streams'] = []
    manager = bytearray.fromhex(base['manager_raw']['raw_hex'])
    struct.pack_into('<3I', manager, 0, 0x11fec8, 0x120ad0, 0x120b48)
    for i in range(2):
        struct.pack_into('<I', manager, 0x40 + i * 0xe0 + 0x50, 0x50)
    base['manager_raw'] = raw_wrapper(base['manager'], manager)
    for i in range(2):
        at = 0x40 + i * 0xe0
        base[f'layer{i}'] = raw_wrapper(base['manager'] + at, manager[at:at + 0xe0])
    body = bytearray(0x81d0)
    struct.pack_into('<I', body, 0x8060, base['manager'])
    base['battle_root'] = raw_wrapper(base['root'], body)
    native_hud['execution'] = 'isolated'
    # Two changing primary-color writes, one for each raw color selector.
    for i in range(2):
        record = tool.hud.GROUPS_OFFSET + (tool.hud.GROUP_RECORD_OFFSETS[2] + i) * 68
        for wi, value in [(8, 7), (9, 1), (10, i)]:
            put(base['ruw_slot0']['raw_owner'], record + wi * 4, value)
            native_hud['groups'][2][i]['words'][wi] = value
    for i in range(2):
        w = base['controller_windows'][i]['raw']
        for at, value in [(0x2698, tool.win.STREAMS[i][0]), (0x26c, 0x3f800001),
                          (0x1900, 0xffffffff), (0x650, 0xb00000 + i * 0x10000),
                          (0x654, 0xc00000 + i * 0x10000)]:
            put(w, at, value)
        for at in (0xcc0, 0xcc4, 0xce8, 0x264, 0x260, 0xcd0, 0x3be4): put(w, at, 0x11223344)
    post = copy.deepcopy(base)
    native = {'schema': tool.SCHEMA, 'scope': tool.SCOPE, 'windows': [], 'hud': native_hud}
    for slot in range(2):
        wrapper = post['controller_windows'][slot]['raw']; p = wrapper['address']
        ca, cb = tool.win.STREAMS[slot][1:3]
        records = [(g, i) for g in tool.GROUPS[slot] for i in range(tool.hud.GROUP_COUNTS[g])]
        cursor = 4 + 2 * len(records)
        stream = [14, 0x200b78, 6, 0x200b88]
        commands = [{'opcode': 14, 'binding': 'ruw_metrics'}, {'opcode': 6, 'binding': 'ruw_clip'}]
        for g, i in records:
            off = tool.hud.GROUPS_OFFSET + (tool.hud.GROUP_RECORD_OFFSETS[g] + i) * 68
            stream += [23, {'raw_widget_offset': off}]
            commands.append({'opcode': 23, 'group': g, 'index': i})
        stream.append(0)
        for i, v in enumerate(stream):
            put(wrapper, 0x26dc + i * 4, post['ruw_slot0']['raw_pointer'] + v['raw_widget_offset'] if isinstance(v, dict) else v)
        for at, value in [(0x2698, p + 0x26dc), (0x2edc, cursor), (0x3bec, 2),
                          (0x3be4, 0xffffffff), (0x268, 0x3f800001), (0x258, 6), (0x254, 6),
                          (0x1900, 0xfffff6c5)]: put(wrapper, at, value)
        for at in (0xcc0, 0xcc4, 0xce8, 0x264, 0x260, 0xcd0): put(wrapper, at, 0)
        for i in range(32): put(wrapper, 0x3c08 + i * 4, (5 if i < ca else 0) | (8 if i < cb else 0))
        extent = (ca + 1) * 256
        positions = bytes(extent); metrics = bytearray(extent)
        for i in range(ca * 16): struct.pack_into('<I', metrics, i * 16 + 8, 0xbf800000)
        e = {'slot': slot, 'window': p, 'active_stream_pointer': p + 0x26dc,
             'embedded_stream_active': True, 'cursor': cursor, 'word_count_including_terminator': cursor + 1,
             'words': stream, 'glyph_buffer_capacity': ca, 'glyph_buffer_allocation_count': ca + 1,
             'glyph_buffer_extent_bytes': extent, 'glyph_positions_pointer': 0xb00000 + slot * 0x10000,
             'glyph_metrics_pointer': 0xc00000 + slot * 0x10000}
        e['glyph_positions'] = raw_wrapper(e['glyph_positions_pointer'], positions)
        e['glyph_metrics'] = raw_wrapper(e['glyph_metrics_pointer'], metrics)
        post['embedded_streams'].append(e)
        scalar = list(struct.unpack('<' + 'I' * tool.win.WINDOW_WORDS, bytes.fromhex(wrapper['raw_hex'])))
        scalar[0x2698 // 4] = None
        for i in range(1, cursor, 2): scalar[0x26dc // 4 + i] = None
        native['windows'].append({'slot': slot, 'scalar_words': scalar, 'commands': commands,
                                 'glyph_positions_hex': positions.hex(), 'glyph_metrics_hex': metrics.hex()})
    for i in range(2):
        n = native_hud['groups'][2][i]
        target = n['words'][2 + i]
        n['runtime'][4] = target
        at = (tool.hud.GROUP_RECORD_OFFSETS[2] + i) * 44 + 16
        put(post['ruw_slot0']['runtime_owner'], at, target)
    snapshots = []
    for i in range(8):
        r = copy.deepcopy(base if i == 0 else post)
        kind, pc = tool.CALLBACKS[(i // 2) % 2]
        r.update(event=kind + '_callback_' + ('complete' if i % 2 else 'entry'),
                 callback=hex(pc), sequence=i, frame=127 + i // 4,
                 pc=0x1248a0 if i % 2 else pc, ra=0x1248a0, caller_return_pc=0x1248a0,
                 post_callback=bool(i % 2))
        snapshots.append(r)
    return [{'schema': 'okage-battle-ui-updates-v1', 'elf_sha256': tool.win.ELF_SHA256}, *snapshots,
            {'event': 'finish', 'first_entered': 2, 'first_completed': 2,
             'second_entered': 2, 'second_completed': 2, 'snapshots': 8}], native

class VerifyUiBindingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.ref, self.native = fixture()

    def load(self):
        path = self.path / 'original.jsonl'
        path.write_text(''.join(json.dumps(r) + '\n' for r in self.ref))
        return tool.load_reference(path)

    def compare(self): return tool.compare(self.load(), self.native)

    def test_exact_binding_and_independent_source_assertions(self):
        report = self.compare()
        self.assertEqual(report['status'], 'pass')
        self.assertEqual(report['compared_glyph_bytes'], 22528)
        self.assertEqual(report['eligible_color_writes'], 2)
        self.assertEqual(report['source_expected_changed_color_values'], 2)
        self.assertEqual(report['compared_reset_and_flag_words'], 90)
        self.assertFalse(report['fcsr_compared_to_native'])

    def test_callback_fields_are_outside_binding_scope(self):
        for slot in range(2):
            w = self.ref[2]['controller_windows'][slot]['raw']
            for at in (0x270, 0x274, 0x278, 0x27c): put(w, at, 0xabababab)
        put(self.ref[2]['controller_windows'][1]['raw'], 0x3f4, 0x3fabc123)
        self.ref[2]['fcsr'] = 0x1000000
        self.assertEqual(self.compare()['status'], 'pass')
        self.assertEqual(self.compare()['reference_fcsr'][1], 0x1000000)

    def test_identical_corruption_both_sides_still_fails(self):
        put(self.ref[2]['controller_windows'][0]['raw'], 0xcc0, 1)
        self.native['windows'][0]['scalar_words'][0xcc0 // 4] = 1
        report = self.compare()
        self.assertEqual(report['status'], 'fail')
        self.assertEqual(report['difference_count'], 2)

    def test_full_glyph_tail_and_metrics_are_checked(self):
        for key in ('glyph_positions_hex', 'glyph_metrics_hex'):
            with self.subTest(key=key):
                ref, native = fixture(); self.ref, self.native = ref, native
                original = self.native['windows'][1][key]
                self.native['windows'][1][key] = original[:-2] + '01'
                self.assertEqual(self.compare()['status'], 'fail')
        self.native['windows'][1]['glyph_metrics_hex'] = '00'
        with self.assertRaises(tool.Invalid): self.compare()

    def test_wrong_widget_order_or_pointer_is_rejected(self):
        self.native['windows'][0]['commands'][2]['index'] = 1
        with self.assertRaises(tool.Invalid): self.compare()
        self.ref, self.native = fixture()
        self.ref[2]['embedded_streams'][0]['words'][5]['raw_widget_offset'] += 68
        with self.assertRaises(tool.Invalid): self.compare()
        self.ref, self.native = fixture()
        self.native['windows'][0]['scalar_words'][0x26e0 // 4] = 0x200b78
        with self.assertRaises(tool.Invalid): self.compare()

    def test_primary_color_change_and_preservation_checked(self):
        self.native['hud']['groups'][2][0]['runtime'][4] ^= 1
        self.assertEqual(self.compare()['status'], 'fail')
        self.ref, self.native = fixture()
        at = tool.hud.GROUP_RECORD_OFFSETS[2] * 44 + 20
        put(self.ref[2]['ruw_slot0']['runtime_owner'], at, 123)
        self.assertEqual(self.compare()['status'], 'fail')

    def test_partial_duplicate_finish_pair_and_owner_errors(self):
        mutations = [lambda r: r.pop(4), lambda r: r[4].update(sequence=2),
                     lambda r: r[-1].update(first_completed=1), lambda r: r[2].update(sp=0),
                     lambda r: r[3].update(callback='0x11fec8'),
                     lambda r: r[2].update(root=r[2]['root'] + 16)]
        for mutate in mutations:
            self.ref, self.native = fixture(); mutate(self.ref)
            with self.assertRaises(tool.Invalid): self.compare()

    def test_schema_bool_extent_and_runtime_pointer_reject(self):
        mutations = [lambda n: n['windows'][0].update(slot=True),
                     lambda n: n['windows'][0]['scalar_words'].pop(),
                     lambda n: n['windows'][0]['commands'][2].update(index=True),
                     lambda n: n['hud']['groups'][2][0]['words'].__setitem__(16, 0x100000),
                     lambda n: n['hud']['groups'][2][0].update(runtime=None)]
        for mutate in mutations:
            self.ref, self.native = fixture(); mutate(self.native)
            with self.assertRaises(tool.Invalid): self.compare()

    def test_cli_hashes_duplicate_json_and_nonclobber(self):
        self.load(); native = self.path / 'native.json'; native.write_text(json.dumps(self.native))
        output = self.path / 'report.json'
        args = [str(self.path / 'original.jsonl'), str(native), '--output', str(output)]
        self.assertEqual(tool.main(args), 0)
        report = json.loads(output.read_text())
        self.assertEqual(len(report['inputs']['original']['sha256']), 64)
        before = output.read_bytes()
        self.assertEqual(tool.main(args), 2)
        self.assertEqual(output.read_bytes(), before)
        native.write_text('{"schema":1,"schema":2}')
        args[-1] = str(self.path / 'duplicate.json')
        self.assertEqual(tool.main(args), 2)
        self.assertFalse(Path(args[-1]).exists())

if __name__ == '__main__': unittest.main()
