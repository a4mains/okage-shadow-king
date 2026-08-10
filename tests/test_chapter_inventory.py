"""chapter_inventory: exact FLDSET/PME parsing, all-branch message controls,
and validated (candidate-labelled) XMT scan, with negative controls."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import chapter_inventory as ci  # noqa: E402

def pme(strings, nodes):
    out = bytearray(b'KOME_RXUI_DATA  ') + struct.pack('<I', len(strings))
    for i, s in enumerate(strings):
        out += struct.pack('<II', i, len(s) + 1)
    out += struct.pack('<I', len(nodes))
    for depth, index in nodes:
        out += struct.pack('<II', depth, index)
    for s in strings:
        out += s + b'\0'
    return bytes(out)

def fldset(rows):
    """rows: list of (name, [13+ words with string refs as str])."""
    strings = bytearray()
    offsets = {}

    def ref(s):
        if s not in offsets:
            offsets[s] = len(strings)
            strings.extend(s.encode() + b'\0')
        return offsets[s]
    records = bytearray()
    table = bytearray()
    for name, words in rows:
        table += struct.pack('<IIII', ref(name), len(records), len(words), 0)
        for w in words:
            records += struct.pack('<I', ref(w) if isinstance(w, str) else w & 0xffffffff)
    head = struct.pack('<IIIII', 0x424458, len(records), len(strings), len(rows), 0xffffffff) + bytes(12)
    return head + table + records + strings

def scene_words(party=0, startup=60, room='field_1d', message='field', event='e_field'):
    return [party, 0, 0, startup, 'map/field', room, message, event, 0, 0, 7, -1, 0]

def xmt(entries_code, code_words):
    """entries_code: list of code offsets (relative to code start) per entry."""
    n = len(entries_code)
    table_end = 32 + n * 8
    head = b'XMT\0' + bytes(28)
    table = bytearray()
    for i, rel in enumerate(entries_code):
        payload = 32 + i * 8 + 4
        table += struct.pack('<II', 1, table_end + rel - payload)
    return head + table + b''.join(struct.pack('<I', w & 0xffffffff) for w in code_words)

def word(op, a=0, b=0):
    return (op & 0xff) | ((a & 0xfff) << 8) | ((b & 0xfff) << 20)

class FldsetTests(unittest.TestCase):
    def test_scene_rows_and_npcs(self):
        npc = ['npc_a', 3, 21020, -1, 0, 0, 0, 0]
        data = fldset([('scene1', scene_words()), ('scene14', scene_words(room='gear1a', message='mado_in',
                                                                         event='e_MADO1Fa')[:12] + npc + [0]),
                       ('other', [0] * 13)])
        scenes = ci.parse_fldset(data)
        self.assertEqual(sorted(scenes), [1, 14])
        self.assertEqual((scenes[14]['message'], scenes[14]['event'], scenes[14]['npcs'][0]['talk']),
                         ('mado_in', 'e_MADO1Fa', 21020))
        self.assertEqual(scenes[1]['secondary_music'], -1)

    def test_rejects_bad_header_and_missing_terminator(self):
        with self.assertRaises(ValueError):
            ci.parse_fldset(b'\0' * 64)
        with self.assertRaises(ValueError):
            ci.parse_fldset(fldset([('scene2', scene_words()[:12] + ['m', 1, 2, 3, 0, 0, 0, 0])]))

    def test_rejects_duplicate_scene(self):
        with self.assertRaises(ValueError):
            ci.parse_fldset(fldset([('scene3', scene_words()), ('scene3', scene_words())]))

class PmeTests(unittest.TestCase):
    def setUp(self):
        strings = [b'', b'21020\r\n', b'Go \\y[SET(10/4/11)SET(125/1/1)]', b'branch two \\y[SET(0/2/8)]',
                   b'note 99', b'21021\r\n', b'plain', b'header 5']
        nodes = [(0, 0), (0, 1), (1, 2), (2, 3), (1, 4), (0, 5), (1, 6), (0, 7), (1, 6)]
        self.data = pme(strings, nodes)

    def test_messages_include_every_branch(self):
        strings, nodes = ci.parse_pme(self.data)
        msgs = ci.pme_messages(strings, nodes)
        self.assertEqual(sorted(msgs), [21020, 21021])
        controls = ci.message_controls(strings, msgs[21020])
        self.assertEqual([(c['op'], c['index'], c['bank'], c['value']) for c in controls],
                         [('SET', 10, 4, 11), ('SET', 125, 1, 1), ('SET', 0, 2, 8)])
        self.assertEqual(ci.message_controls(strings, msgs[21021]), [])

    def test_non_numeric_root_ends_the_previous_message(self):
        strings, nodes = ci.parse_pme(self.data)
        msgs = ci.pme_messages(strings, nodes)
        self.assertEqual(msgs[21021], [6])

    def test_rejects_corrupt_bank(self):
        with self.assertRaises(ValueError):
            ci.parse_pme(self.data[:-1])
        with self.assertRaises(ValueError):
            ci.parse_pme(b'KOME_RXUI_DATA  ' + struct.pack('<I', 1) + struct.pack('<II', 1, 1) + bytes(8))

class XmtTests(unittest.TestCase):
    scenes = {1: {}, 14: {}}

    def test_validated_candidates(self):
        code = [word(0x0b, 1, 0x10), 0x0004000a, 61,      # IF b4[10] == 61
                word(0x47, 0, 11), 0x0004000a,            # SET b4[10] = 11
                word(0x4c), 21020,                        # message 21020
                word(0x43, 1, 14),                        # scene 14 entrance 1
                word(0x43, 1, 15),                        # scene 15 does not exist: rejected
                word(0x4c), 99999,                        # unknown message: rejected
                word(0x47, 0, 1), 0x00090001,             # bank 9: rejected
                word(0x62, 0, 12), 0,                     # Save request
                word(0x62, 0, 3), 0,                      # unrecovered mode: not listed
                0]
        scan = ci.scan_xmt(xmt([0, 12], code), self.scenes, {21020})
        kinds = [(h['kind'], h.get('scene') or h.get('message') or h.get('value')) for h in scan['candidates']]
        self.assertEqual(kinds, [('if', 61), ('set', 11), ('message', 21020), ('scene_request', 14),
                                 ('save_request', 0)])
        self.assertTrue(all(h['label'] == 'candidate' for h in scan['candidates']))
        iff = scan['candidates'][0]
        self.assertEqual((iff['bank'], iff['index'], iff['mode'], iff['false_target']),
                         (4, 10, '==', iff['pc'] + 16))
        self.assertEqual([h['entry_hint'] for h in scan['candidates']], [0, 1, 1, 1, 1])

    def test_rejects_bad_entry_table(self):
        with self.assertRaises(ValueError):
            ci.xmt_entries(b'XMT\0' + bytes(28) + struct.pack('<II', 1, 0x7ffffff0))
        with self.assertRaises(ValueError):
            ci.xmt_entries(b'XMZ\0' + bytes(40))

class StateTests(unittest.TestCase):
    def test_save_and_variables_agree(self):
        v = bytearray(0xd78)
        v[0x210] = 7
        struct.pack_into('<i', v, 0x510, 74)
        struct.pack_into('<i', v, 0x538, 10)
        v[0x110 + 125 // 8] |= 1 << (7 - 125 % 8)
        with tempfile.TemporaryDirectory() as d:
            save = Path(d) / 'bkmo2.dat'
            save.write_bytes(b'!crc' + bytes(8) + bytes(v))
            raw = Path(d) / 'x.variables.bin'
            raw.write_bytes(bytes(v) + bytes(0x1000 - 0xd78))
            for p in (save, raw):
                s = ci.progress_state(ci.variables_from(p))
                self.assertEqual((s['story'], s['scene'], s['objective']), (7, 74, 10))
            self.assertEqual(ci.bit_flag(ci.variables_from(save), 1, 125), 1)
            self.assertEqual(ci.bit_flag(ci.variables_from(save), 1, 124), 0)
            (Path(d) / 'short.dat').write_bytes(b'!crc' + bytes(20))
            with self.assertRaises(ValueError):
                ci.variables_from(Path(d) / 'short.dat')

if __name__ == '__main__':
    unittest.main()
