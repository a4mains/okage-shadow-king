"""Synthetic strict field-prefix controls; never retail fixtures or runtime inputs.

Unit fixtures shorten the RNG stream to 12 draws and VRAM to 64 bytes. The
production cold-route count and full-size masks are separately exercised against
retained original/native sessions; no production module globals are modified.
"""
import hashlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import field_prefix_compare as cmp

def dump(path, obj):
    path.write_text(json.dumps(obj))

def jl(rows):
    return ''.join(json.dumps(r) + '\n' for r in rows)

class Fixture:
    def __init__(self, root):
        self.orig, self.native, self.pins = root / 'original', root / 'native', root / 'pins.json'
        self.orig.mkdir()
        self.native.mkdir()
        rng, seed = [], 1
        for i in range(cmp.RNG_COUNT):
            seed = (seed * 1103515245 + 12345) & 0xffffffff
            rng.append({'i': i, 'phase': 'session', 'state': seed, 'value': seed & 0x7fffffff})
        self.rng = rng
        fields = [dict(kind='field', op='resource-create'), dict(kind='field', op='switch-337e70', value=1),
                  dict(kind='field', op='job-enqueue', job='0x132688', scene=72, arg=0),
                  dict(kind='field', op='worker-entry'), dict(kind='field', op='clut-class', value=0),
                  dict(kind='field', op='lower-case', value=1), dict(kind='field', op='atlas', handle=36),
                  dict(kind='field', op='texenv', handle=37),
                  dict(kind='field', op='env-bind', handle=37, tex0='0x2004d826a930a842', region=[1, 2, 3, 4]),
                  dict(kind='field', op='ruw', slot=0, bytes=10, sha256='a' * 64),
                  dict(kind='field', op='ui-available')]
        filler = {'kind': 'glyph', 'op': 'cache-transfer'}
        events, fi = [filler] * 1268, 0
        self.ref = {'schema': 'okage.field-prefix-original-reference.v1', 'route': cmp.ROUTE, 'checkpoints': {},
                    'rng': {'provenance': 'every draw is a hooked observation', 'total': cmp.RNG_COUNT, 'window_draws': 0}, 'problems': [],
                    'control_minimal': {'failures': [], 'missing': [], 'checkpoints_compared': {
                        cp: {'resource_state_equal': {'handles': True}, 'session_state_equal': True,
                             'field_equal_except_counter': {'w34': True}, 'vram_fill00_equal': True,
                             **({'ee_ram_equal': True} if cp.startswith('ui-publish') else {})}
                        for cp in ('field-constructor-return', 'worker-entry', 'ui-publish-pre', 'ui-publish-post-block')}},
                    'newgame_neutrality': {cp: {k: True for k in ('events.jsonl', 'state.json', 'rng.jsonl', 'session.json', 'effects.json')}
                                      for cp in cmp.h.CHECKPOINTS},
                    'publish_proof': {'stores': {'0x34': {'before': 0, 'after': 1, 'expected': 1, 'ok': True}},
                                      'resource_state_equal': True, 'vram_equal': True, 'changed_words_outside_block_count': 0},
                    'determinism': {f'checkpoint {cp}: {suffix}': {'identical': True}
                        for cp in cmp.CHECKPOINTS[:-1] + ('ui-publish-pre',)
                        for suffix in ('state (fill fields removed)', 'vram-last-writer.bin', 'vram-upload-ever.bin', 'vram.bin within fill')},
                    'scheduling': {'field_constructor_entry': {'vb': 100, 'traversal': 100},
                                   'checkpoints': {cp: {'vb': 100 + i, 'traversal': 100 + i} for i, cp in enumerate(cmp.CHECKPOINTS)}}}
        mask, values, invalid = bytearray(cmp.VRAM), bytearray(cmp.VRAM), bytearray(cmp.VRAM)
        mask[16], values[16], invalid[32] = 3, 0x5a, 3
        bins = {'vram-written-mask.bin': bytes(mask), 'vram-written-values.bin': bytes(values), 'vram-invalidated-mask.bin': bytes(invalid)}
        for i, cp in enumerate(cmp.CHECKPOINTS):
            count = cmp.FIELD_COUNTS[cp]
            events += fields[fi:count]
            fi = count
            events += [filler] * (cmp.EVENT_COUNTS[cp] - len(events))
            field = {**{k: 0 for k in cmp.FIELD_INT}, **{k: False for k in cmp.FIELD_BOOL},
                     'resource_created': True, 'job_queued': True, 'switch_337e70': 1, 'scene': 72,
                     'w34': int(i == 6), 'manager_60': 36, 'resource_48': 37, 'tex0_332430': '0x2004d826a930a842',
                     'region_332450': [1, 2, 3, 4], 'ruw_sha256': None, 'ruw_slot0_live': i >= 5}
            state = {'coverage': {'lifecycle': 'field-prefix', 'checkpoint': cp, 'memory': cmp.MEMORY},
                     'allocations': [], 'handles': [], 'records': [], 'hash': [], 'reserved': [],
                     'free_records': 512, 'free_extras': 128, 'available': 4096, 'cursors': [0] * 64,
                     'free_queue_ids': [], 'occupancy_hex': '00' * 8192, 'image_list': [], 'materials': []}
            session = {'image_counter': 0, 'command': [0, 0, 0], 'name_844': 'Hero' + '\0' * 28, 'temp_10_sha256': 'b' * 64,
                       'fade': dict.fromkeys(('current', 'target', 'rate', 'color'), '0x0'),
                       'mode_parent': {'phase': 0, 'queue': [0, 0, 0], 'current': [2, 72, 0]}}
            for root in (self.orig, self.native):
                d = root / cp
                d.mkdir()
                (d / ('events.jsonl' if root == self.orig else 'lifecycle.jsonl')).write_text(jl(events))
                (d / 'rng.jsonl').write_text(jl(rng))
                dump(d / 'field.json', field)
                dump(d / 'state.json', state)
                dump(d / 'session.json', session)
                dump(d / 'effects.json', {'effect106': {'slot': 0, 'shape': None, 'state': 'reclaimed', 'element_handles': []}})
                for name, data in bins.items():
                    nn = dict(zip(('vram-written-mask.bin', 'vram-written-values.bin', 'vram-invalidated-mask.bin'),
                                  ('vram.known', 'vram.bytes', 'vram.invalidated')))[name]
                    (d / (name if root == self.orig else nn)).write_bytes(data)
            vd = {k: hashlib.sha256(bins[n]).hexdigest() for n, k in zip(bins, ('mask_sha256', 'values_sha256', 'invalidated_sha256'))}
            self.ref['checkpoints'][cp] = {'resource_events': len(events), 'rng_draws_before': len(rng), 'field': field,
                                          'vb': 100 + i, 'traversal': 100 + i, 'vram': {**vd, 'known_nibbles_required': 2, 'unknown_nibbles_required': 2}}
        dump(self.orig / 'reference.json', self.ref)
        dump(self.native / 'field-route.json', {'route': cmp.ROUTE, 'elf_sha256': cmp.ELF,
                                               'checkpoints': {**{cp: 100 for cp in cmp.h.CHECKPOINTS}, **{cp: 100 + i for i, cp in enumerate(cmp.CHECKPOINTS)}}})
        self.repin()

    def repin(self):
        files = ['reference.json'] + [f'{cp}/{f}' for cp in cmp.CHECKPOINTS for f in cmp.ORIGINAL_FILES]
        dump(self.pins, {'schema': 'okage.field-prefix-pins.v1', 'elf_sha256': cmp.ELF,
                         'checkpoints': list(cmp.CHECKPOINTS), 'files': {f: {'bytes': len(b := (self.orig / f).read_bytes()),
                         'sha256': hashlib.sha256(b).hexdigest()} for f in files}})

    def json_change(self, root, cp, name, fn):
        p = root / cp / name if cp else root / name
        obj = json.loads(p.read_text())
        fn(obj)
        dump(p, obj)

    def rows_change(self, root, cp, name, fn):
        p = root / cp / name
        rows = [json.loads(x) for x in p.read_text().splitlines()]
        fn(rows)
        p.write_text(jl(rows))

class Comparator(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.size = patch.object(cmp, 'VRAM', 64)
        self.size.start()
        self.addCleanup(self.size.stop)
        short_rng = patch.object(cmp, 'RNG_COUNT', 12)
        short_rng.start()
        self.addCleanup(short_rng.stop)
        self.f = Fixture(Path(self.tmp.name))
        self.cp = cmp.CHECKPOINTS[-1]

    def run_cmp(self):
        return cmp.compare(self.f.orig, self.f.native, self.f.pins, vram_size=64)

    def test_equal(self):
        code, result = self.run_cmp()
        self.assertEqual(code, 0)
        self.assertTrue(result['payload_identical'])
        self.assertFalse(result['whole_timeline_parity'])

    def test_scheduling_is_retained_without_claiming_timeline_parity(self):
        self.f.json_change(self.f.native, None, 'field-route.json', lambda x: x['checkpoints'].update({cp: 300 for cp in cmp.CHECKPOINTS}))
        self.assertEqual(self.run_cmp()[0], 0)

    def test_missing_native_endpoint(self):
        (self.f.native / self.cp / 'rng.jsonl').unlink()
        self.assertEqual(self.run_cmp()[0], 5)

    def test_missing_native_field(self):
        self.f.json_change(self.f.native, self.cp, 'field.json', lambda x: x.pop('reserve_class_3abb14'))
        self.assertEqual(self.run_cmp()[0], 5)

    def test_common_missing_field_never_passes(self):
        for d in (self.f.orig, self.f.native):
            self.f.json_change(d, self.cp, 'field.json', lambda x: x.pop('reserve_class_3abb14'))
        self.f.repin()
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_native_missing_lifecycle(self):
        self.f.rows_change(self.f.native, self.cp, 'lifecycle.jsonl', lambda x: x.pop())
        self.assertEqual(self.run_cmp()[0], 5)

    def test_common_truncated_lifecycle(self):
        for root, name in ((self.f.orig, 'events.jsonl'), (self.f.native, 'lifecycle.jsonl')):
            self.f.rows_change(root, self.cp, name, lambda x: x.pop())
        self.f.repin()
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_duplicate_field_op_same_count(self):
        self.f.rows_change(self.f.native, self.cp, 'lifecycle.jsonl', lambda x: x.__setitem__(-1, x[-2]))
        self.assertEqual(self.run_cmp()[0], 5)

    def test_reordered_field_ops_same_count(self):
        def swap(x):
            x[-1], x[-2] = x[-2], x[-1]
        self.f.rows_change(self.f.native, self.cp, 'lifecycle.jsonl', swap)
        self.assertEqual(self.run_cmp()[0], 5)

    def test_asserted_rows_unproven(self):
        self.f.rows_change(self.f.native, self.cp, 'lifecycle.jsonl', lambda x: x[0].update(asserted=True))
        self.assertEqual(self.run_cmp()[0], 4)

    def test_failed_reference_control_unproven(self):
        self.f.ref['problems'] = ['control:ui-publish-pre:ee_ram_equal']
        self.f.ref['control_minimal']['failures'] = ['ui-publish-pre:ee_ram_equal']
        self.f.ref['control_minimal']['checkpoints_compared']['ui-publish-pre']['ee_ram_equal'] = False
        dump(self.f.orig / 'reference.json', self.f.ref)
        self.f.repin()
        self.assertEqual(self.run_cmp()[0], 4)

    def test_failure_records_disagree(self):
        self.f.ref['problems'] = ['control:ee_ram_equal']
        dump(self.f.orig / 'reference.json', self.f.ref)
        self.f.repin()
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_missing_reference_control_endpoints(self):
        self.f.ref['control_minimal']['missing'] = ['atlas-loaded']
        dump(self.f.orig / 'reference.json', self.f.ref)
        self.f.repin()
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_publish_proof_corrupted(self):
        self.f.ref['publish_proof']['stores']['0x34']['after'] = 0
        dump(self.f.orig / 'reference.json', self.f.ref)
        self.f.repin()
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_corrupted_descriptor(self):
        self.f.json_change(self.f.native, self.cp, 'field.json', lambda x: x.update(tex0_332430='0x0000000000000001'))
        self.assertEqual(self.run_cmp()[0], 1)

    def test_corrupted_session(self):
        self.f.json_change(self.f.native, self.cp, 'session.json', lambda x: x.update(temp_10_sha256='c' * 64))
        self.assertEqual(self.run_cmp()[0], 1)

    def test_corrupted_state(self):
        self.f.json_change(self.f.native, self.cp, 'state.json', lambda x: x.update(free_extras=127))
        self.assertEqual(self.run_cmp()[0], 1)

    def test_corrupted_rng(self):
        self.f.rows_change(self.f.native, self.cp, 'rng.jsonl', lambda x: x[10].update(value=1))
        self.assertEqual(self.run_cmp()[0], 5)

    def test_common_corrupted_rng(self):
        for root in (self.f.orig, self.f.native):
            self.f.rows_change(root, self.cp, 'rng.jsonl', lambda x: x[10].update(value=1))
        self.f.repin()
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_original_unpinned(self):
        (self.f.orig / self.cp / 'field.json').write_text('{}')
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_wrong_elf(self):
        self.f.json_change(self.f.native, None, 'field-route.json', lambda x: x.update(elf_sha256='f' * 64))
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_invalid_mask_bits(self):
        p = self.f.native / self.cp / 'vram.known'
        p.write_bytes(bytes([4]) + bytes(63))
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_mask_gap_is_difference(self):
        p = self.f.native / self.cp / 'vram.known'
        b = bytearray(p.read_bytes()); b[16] = 0; p.write_bytes(b)
        p = self.f.native / self.cp / 'vram.invalidated'
        b = bytearray(p.read_bytes()); b[16] = 3; p.write_bytes(b)
        self.assertEqual(self.run_cmp()[0], 1)

    def test_common_mask_gap_digest_refuses(self):
        for root, name in ((self.f.orig, 'vram-written-mask.bin'), (self.f.native, 'vram.known')):
            p = root / self.cp / name
            b = bytearray(p.read_bytes()); b[16] = 0; p.write_bytes(b)
        self.f.repin()
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_invalidated_mask_exact_even_never_region(self):
        p = self.f.native / self.cp / 'vram.invalidated'
        b = bytearray(p.read_bytes()); b[40] = 1; p.write_bytes(b)
        self.assertEqual(self.run_cmp()[0], 1)

    def test_defined_lane_corruption(self):
        p = self.f.native / self.cp / 'vram.bytes'
        b = bytearray(p.read_bytes()); b[16] ^= 0x10; p.write_bytes(b)
        self.assertEqual(self.run_cmp()[0], 1)

    def test_undefined_lane_ignored(self):
        p = self.f.native / self.cp / 'vram.bytes'
        b = bytearray(p.read_bytes()); b[40] = 255; p.write_bytes(b)
        self.assertEqual(self.run_cmp()[0], 0)

    def test_control_common_missing_checkpoint_evidence(self):
        self.f.ref['control_minimal']['checkpoints_compared'].pop('worker-entry')
        dump(self.f.orig / 'reference.json', self.f.ref)
        self.f.repin()
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_control_failure_without_witness(self):
        self.f.ref['control_minimal']['failures'] = ['ui-publish-pre:ee_ram_equal']
        self.f.ref['problems'] = ['control:ui-publish-pre:ee_ram_equal']
        dump(self.f.orig / 'reference.json', self.f.ref)
        self.f.repin()
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_determinism_common_missing_checkpoint(self):
        self.f.ref['determinism'].pop('checkpoint worker-entry: vram-last-writer.bin')
        dump(self.f.orig / 'reference.json', self.f.ref)
        self.f.repin()
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_native_shared_inode_rejected(self):
        p = self.f.native / self.cp / 'field.json'
        p.unlink()
        os.link(self.f.orig / self.cp / 'field.json', p)
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_truncated_vram_rejected(self):
        (self.f.native / self.cp / 'vram.bytes').write_bytes(bytes(63))
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_duplicate_json_key_rejected(self):
        (self.f.native / self.cp / 'field.json').write_text('{"w30":0,"w30":1}')
        with self.assertRaises(cmp.Malformed):
            self.run_cmp()

    def test_malformed_api_returns_exit_two(self):
        (self.f.native / self.cp / 'field.json').write_text('{}')
        self.assertEqual(cmp.run(self.f.orig, self.f.native, self.f.pins, vram_size=64)[0], 2)

    def test_self_compare_refused(self):
        with self.assertRaises(cmp.Malformed):
            cmp.compare(self.f.orig, self.f.orig, self.f.pins, vram_size=64)

    def test_imported_module_globals_unchanged(self):
        before = cmp.base.VRAM, cmp.base.read_text, cmp.g.VRAM, cmp.h.CHECKPOINTS
        self.run_cmp()
        self.assertEqual(before, (cmp.base.VRAM, cmp.base.read_text, cmp.g.VRAM, cmp.h.CHECKPOINTS))

if __name__ == '__main__':
    unittest.main()
