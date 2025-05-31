"""Controls for tools/title_compare.py and tools/title_original_extract.py.

Synthetic fixtures exercise every comparator rule (each negative control must fail with the right exit
status); optional checks run against the ignored, pinned Title original evidence when it is present
(set OKAGE_MAIN_CHECKOUT to the main checkout from a worktree).
"""
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import title_compare as cmp  # noqa: E402
import title_original_extract as ext  # noqa: E402

MAIN = Path(os.environ.get('OKAGE_MAIN_CHECKOUT', ROOT))
EVIDENCE = MAIN / 'local/research/title-original-v1/analysis'
VRAM = 1 << 22

EVENTS = [
    {'kind': 'phase', 'name': 'prefix-end-0x100618'},
    {'kind': 'alloc', 'alloc': 0, 'class': 1, 'psm': 1, 'w': 32, 'h': 16, 'tbp': 100},
    {'kind': 'ref', 'alloc': 0, 'delta': 1, 'refs': 1},
    {'kind': 'reserve', 'count': 2, 'first': 5, 'class': 0},
    {'kind': 'handle', 'handle': 5, 'record': 7, 'alias': False},
    {'kind': 'upload', 'alloc': 0},
    {'kind': 'bind', 'handle': 5, 'level': 0, 'alloc': -1},
    {'kind': 'name', 'handle': 5, 'name': 'a.xim'},
    {'kind': 'material', 'model': 'NIGHT_V/a.xmd', 'slot': 0, 'handle': 5},
    {'kind': 'retire', 'handle': 5},
    {'kind': 'ref', 'alloc': 0, 'delta': -1, 'refs': 0},
    {'kind': 'free', 'alloc': 0},
    {'kind': 'phase', 'name': 'stop-title-constructor-return'},
]

def state():
    return {
        'coverage': dict(cmp.COVERAGE),
        'allocations': [{'id': 1, 'serial': 2, 'class': 1, 'psm': 1, 'width': 32, 'height': 16, 'tbp': 100, 'tbw': 1,
                         'references': 1, 'descriptor': 12345}],
        'handles': [{'handle': 6, 'record': 8, 'image': [1, 2], 'palettes': [None]}],
        'records': [{'record': 8, 'name': 'b.xim', 'hash_slot': 3, 'handles': 1}],
        'hash': [[3, 16]], 'reserved': [], 'free_records': 511, 'free_extras': 128, 'available': 4095,
        'cursors': [0] * 64, 'free_queue_ids': [0, 2, 3], 'occupancy_hex': '00' * 8192,
        'image_list': [6], 'materials': [[0, 6]],
    }

def jl(rows):
    return ''.join(json.dumps(r) + '\n' for r in rows)

class Fixture:
    """Original analysis dir + manifest pinning it + a matching native dir."""

    def __init__(self, tmp):
        self.tmp = Path(tmp)
        self.orig = self.tmp / 'orig'
        self.nat = self.tmp / 'native'
        self.orig.mkdir()
        self.nat.mkdir()
        mask = bytearray(VRAM)
        vals = bytearray(VRAM)
        inval = bytearray(VRAM)
        mask[0x100] = 3
        vals[0x100] = 0x5a
        mask[0x101] = 1
        vals[0x101] = 0x07
        inval[0x200] = 3
        inval[0x201] = 2
        self.mask, self.vals, self.inval = mask, vals, inval
        (self.orig / 'events.jsonl').write_text(jl(EVENTS))
        (self.orig / 'final-state.json').write_text(json.dumps(state()))
        (self.orig / 'vram-written-mask.bin').write_bytes(bytes(mask))
        (self.orig / 'vram-written-values.bin').write_bytes(bytes(vals))
        (self.orig / 'vram-invalidated-mask.bin').write_bytes(bytes(inval))
        (self.orig / 'gs-writers.json').write_text('{}\n')
        self.manifest = self.tmp / 'manifest.json'
        self.pin()
        (self.nat / 'timeline.jsonl').write_text(jl(EVENTS))
        (self.nat / 'state.json').write_text(json.dumps(state()))
        known = bytearray(mask)
        data = bytearray(VRAM)
        data[0x100] = 0x5a
        data[0x101] = 0xf7
        (self.nat / 'vram.known').write_bytes(bytes(known))
        (self.nat / 'vram.bytes').write_bytes(bytes(data))

    def pin(self):
        outs = {}
        for n in cmp.ORIGINAL_FILES:
            d = (self.orig / n).read_bytes()
            outs['analysis/' + n] = {'sha256': hashlib.sha256(d).hexdigest(), 'bytes': len(d)}
        self.manifest.write_text(json.dumps({'outputs': outs, 'result_digest': {
            'resource_events': sum(1 for e in EVENTS if e['kind'] != 'phase'), 'vram_known_nibbles': 3,
            'vram_unknown_nibbles': 3}}))

    def run(self, nat=None):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = cmp.main([str(self.orig), str(nat or self.nat), '--manifest', str(self.manifest)])
        return code, out.getvalue() + err.getvalue()

    def edit_timeline(self, fn):
        rows = [json.loads(l) for l in (self.nat / 'timeline.jsonl').read_text().splitlines()]
        rows = fn(rows)
        (self.nat / 'timeline.jsonl').write_text(jl(rows))

    def edit_state(self, fn):
        s = json.loads((self.nat / 'state.json').read_text())
        fn(s)
        (self.nat / 'state.json').write_text(json.dumps(s))

    def edit_vram(self, name, offset, value):
        b = bytearray((self.nat / name).read_bytes())
        b[offset] = value
        (self.nat / name).write_bytes(bytes(b))

class ComparatorControls(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.f = Fixture(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def assertCode(self, code, expect_text=None):
        got, text = self.f.run()
        self.assertEqual(got, code, text)
        if expect_text:
            self.assertIn(expect_text, text)

    def test_identical(self):
        self.assertCode(0, 'RESULT: IDENTICAL')

    def test_phase_markers_are_informational(self):
        self.f.edit_timeline(lambda r: [e for e in r if e['kind'] != 'phase'] + [{'kind': 'phase', 'name': 'native-only'}])
        self.assertCode(0)

    def test_reordered_events_fail(self):
        def swap(r):
            r[2], r[3] = r[3], r[2]
            return r
        self.f.edit_timeline(swap)
        self.assertCode(1, 'timeline: DIFFERENT at resource event 1')

    def test_missing_free_fails(self):
        self.f.edit_timeline(lambda r: [e for e in r if e['kind'] != 'free'])
        self.assertCode(1, 'timeline: DIFFERENT')

    def test_changed_reference_count_fails(self):
        def f(r):
            r[2]['refs'] = 2
            return r
        self.f.edit_timeline(f)
        self.assertCode(1)

    def test_alias_flag_must_be_bool(self):
        def f(r):
            r[4]['alias'] = 0
            return r
        self.f.edit_timeline(f)
        self.assertCode(2, 'handle.alias')

    def test_bool_is_not_an_integer(self):
        def f(r):
            r[1]['class'] = True
            return r
        self.f.edit_timeline(f)
        self.assertCode(2)

    def test_extra_event_key_is_malformed(self):
        def f(r):
            r[5]['tbp'] = 100
            return r
        self.f.edit_timeline(f)
        self.assertCode(2, 'upload keys')

    def test_unknown_event_kind_is_malformed(self):
        self.f.edit_timeline(lambda r: r + [{'kind': 'draw'}])
        self.assertCode(2, 'unknown event kind')

    def test_truncated_timeline_is_malformed(self):
        p = self.f.nat / 'timeline.jsonl'
        p.write_text(p.read_text()[:-5])
        self.assertCode(2)

    def test_empty_timeline_is_malformed(self):
        (self.f.nat / 'timeline.jsonl').write_text('')
        self.assertCode(2, 'no resource events')

    def test_state_difference_fails(self):
        self.f.edit_state(lambda s: s['free_queue_ids'].reverse())
        self.assertCode(1, 'state.free_queue_ids: DIFFERENT')

    def test_material_binding_difference_fails(self):
        self.f.edit_state(lambda s: s.__setitem__('materials', [[0, 7]]))
        self.assertCode(1, 'state.materials: DIFFERENT')

    def test_image_list_difference_fails(self):
        self.f.edit_state(lambda s: s.__setitem__('image_list', []))
        self.assertCode(1, 'state.image_list: DIFFERENT')

    def test_serial_difference_fails(self):
        self.f.edit_state(lambda s: s['allocations'][0].__setitem__('serial', 3))
        self.assertCode(1, 'state.allocations: DIFFERENT')

    def test_wrong_coverage_is_malformed(self):
        self.f.edit_state(lambda s: s.__setitem__('coverage', {'lifecycle': 'title-constructor-return', 'memory': 'complete'}))
        self.assertCode(2, 'coverage')

    def test_missing_state_key_is_malformed(self):
        self.f.edit_state(lambda s: s.pop('free_queue_ids'))
        self.assertCode(2, 'missing keys')

    def test_unknown_state_key_is_malformed(self):
        self.f.edit_state(lambda s: s.__setitem__('vram_bytes', 1))
        self.assertCode(2, 'unknown keys')

    def test_ignored_informational_key_is_accepted(self):
        self.f.edit_state(lambda s: s.__setitem__('later_history', 'unmodeled'))
        self.assertCode(0, 'ignored native keys')

    def test_duplicate_json_key_is_malformed(self):
        p = self.f.nat / 'state.json'
        p.write_text(p.read_text()[:-1] + ', "available": 4095}')
        self.assertCode(2, 'duplicate key')

    def test_short_occupancy_is_malformed(self):
        self.f.edit_state(lambda s: s.__setitem__('occupancy_hex', '00' * 8191))
        self.assertCode(2, 'occupancy_hex')

    def test_missing_knowledge_fails(self):
        self.f.edit_vram('vram.known', 0x100, 1)
        self.assertCode(1, 'missing_knowledge=1')

    def test_value_mismatch_fails(self):
        self.f.edit_vram('vram.bytes', 0x100, 0x5b)
        self.assertCode(1, 'value_mismatch=1')

    def test_stale_known_draw_region_fails(self):
        self.f.edit_vram('vram.known', 0x201, 2)
        self.assertCode(1, 'stale_known=1')

    def test_invented_knowledge_fails(self):
        self.f.edit_vram('vram.known', 0x101, 3)
        self.assertCode(1, 'invented_knowledge=1')

    def test_invented_zero_elsewhere_fails(self):
        self.f.edit_vram('vram.known', 0x3000, 3)
        self.assertCode(1, 'invented_knowledge=2')

    def test_unknown_high_nibble_value_is_not_compared(self):
        self.f.edit_vram('vram.bytes', 0x101, 0x37)
        self.assertCode(0)

    def test_known_mask_extra_bits_are_malformed(self):
        self.f.edit_vram('vram.known', 0x100, 7)
        self.assertCode(2, 'bits other than')

    def test_short_vram_is_malformed(self):
        (self.f.nat / 'vram.bytes').write_bytes(b'\0' * (VRAM - 1))
        self.assertCode(2)

    def test_self_compare_is_refused(self):
        code, text = self.f.run(nat=self.f.orig)
        self.assertEqual(code, 2, text)
        self.assertIn('itself', text)

    def test_hardlinked_original_is_refused(self):
        os.remove(self.f.nat / 'timeline.jsonl')
        os.link(self.f.orig / 'events.jsonl', self.f.nat / 'timeline.jsonl')
        self.assertCode(2, 'self-comparison')

    def test_unpinned_original_is_refused(self):
        (self.f.orig / 'events.jsonl').write_text(jl(EVENTS[:-2]))
        self.assertCode(2, 'does not match the pinned')

    def test_manifest_digest_mismatch_is_refused(self):
        m = json.loads(self.f.manifest.read_text())
        m['result_digest']['resource_events'] += 1
        self.f.manifest.write_text(json.dumps(m))
        self.assertCode(2, 'result digest')

    def test_missing_native_file_is_malformed(self):
        os.remove(self.f.nat / 'vram.known')
        self.assertCode(2)

class ExtractorTimeline(unittest.TestCase):
    """The timeline normalisation of tools/title_original_extract.py on hand-made oracle rows."""

    def rows(self):
        desc = {'raw': '0x40080109606771', 'tbp': 10097, 'id': 150, 'psm': 1, 'w': 32, 'h': 16}
        null = {'raw': '0x0', 'tbp': 0, 'id': 0, 'psm': 0, 'w': 0, 'h': 0}
        return [
            {'i': 0, 'ev': 'enter', 'name': 'alloc', 'class': 1, 'psm': 1, 'w': 32, 'h': 16},
            {'i': 1, 'ev': 'exit', 'name': 'alloc', 'enter': 0, 'result': desc},
            {'i': 2, 'ev': 'enter', 'name': 'handle_create', 'desc': desc},
            {'i': 3, 'ev': 'enter', 'name': 'retain', 'desc': desc, 'delta': 1},
            {'i': 4, 'ev': 'exit', 'name': 'retain', 'enter': 3, 'refs_after': 1},
            {'i': 5, 'ev': 'exit', 'name': 'handle_create', 'enter': 2, 'handle': 44, 'record': 46},
            {'i': 6, 'ev': 'enter', 'name': 'upload', 'desc': desc},
            {'i': 7, 'ev': 'exit', 'name': 'upload', 'enter': 6},
            {'i': 8, 'ev': 'enter', 'name': 'handle_name', 'handle': 44, 'name_str': '5wod_g00.xim'},
            {'i': 9, 'ev': 'enter', 'name': 'handle_name', 'handle': 99, 'name_str': 'unmapped.xim'},
            {'i': 10, 'ev': 'enter', 'name': 'retain', 'desc': null, 'delta': -1},
            {'i': 11, 'ev': 'exit', 'name': 'retain', 'enter': 10, 'refs_after': 0},
            {'i': 12, 'ev': 'enter', 'name': 'free', 'desc': null},
            {'i': 13, 'ev': 'exit', 'name': 'free', 'enter': 12, 'freed': False},
            {'i': 14, 'ev': 'enter', 'name': 'palette_bind', 'handle': 44, 'level': 0, 'desc': null, 'record': 46},
            {'i': 15, 'ev': 'enter', 'name': 'handle_retire', 'handle_or_record': -1, 'record': -1},
            {'i': 16, 'ev': 'exit', 'name': 'handle_retire', 'enter': 15},
        ]

    def test_order_and_normalisation(self):
        tl, detail, counts = ext.timeline(self.rows())
        self.assertEqual(tl, [
            {'kind': 'alloc', 'alloc': 150, 'class': 1, 'psm': 1, 'w': 32, 'h': 16, 'tbp': 10097},
            {'kind': 'ref', 'alloc': 150, 'delta': 1, 'refs': 1},
            {'kind': 'handle', 'handle': 44, 'record': 46, 'alias': False},
            {'kind': 'upload', 'alloc': 150},
            {'kind': 'name', 'handle': 44, 'name': '5wod_g00.xim'},
            {'kind': 'bind', 'handle': 44, 'level': 0, 'alloc': -1},
        ])
        self.assertEqual((counts['ref_null'], counts['free_noop'], counts['name_noop'], counts['retire_noop']), (1, 1, 1, 1))
        self.assertEqual([d['oracle_i'] for d in detail], [1, 4, 5, 6, 8, 14])

@unittest.skipUnless((EVIDENCE / 'events.jsonl').is_file(), 'Title original evidence absent (set OKAGE_MAIN_CHECKOUT)')
class PinnedEvidence(unittest.TestCase):
    """The pinned original passes a round trip through the native schema; edits of it fail."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.nat = Path(self._tmp.name) / 'native'
        self.nat.mkdir()
        shutil.copy(EVIDENCE / 'events.jsonl', self.nat / 'timeline.jsonl')
        shutil.copy(EVIDENCE / 'final-state.json', self.nat / 'state.json')
        shutil.copy(EVIDENCE / 'vram-written-values.bin', self.nat / 'vram.bytes')
        shutil.copy(EVIDENCE / 'vram-written-mask.bin', self.nat / 'vram.known')
        for f in self.nat.iterdir():
            f.chmod(0o644)

    def tearDown(self):
        self._tmp.cleanup()

    def run_cmp(self):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = cmp.main([str(EVIDENCE), str(self.nat)])
        return code, out.getvalue() + err.getvalue()

    def test_round_trip_identical(self):
        code, text = self.run_cmp()
        self.assertEqual(code, 0, text)
        self.assertIn('timeline: identical (796 resource events in order)', text)
        self.assertIn('required_known=2586752, required_unknown=3153920', text)

    def test_first_title_free_dropped_fails(self):
        rows = (self.nat / 'timeline.jsonl').read_text().splitlines()
        i = next(k for k, l in enumerate(rows) if json.loads(l)['kind'] == 'free')
        (self.nat / 'timeline.jsonl').write_text('\n'.join(rows[:i] + rows[i + 1:]) + '\n')
        code, text = self.run_cmp()
        self.assertEqual(code, 1, text)

    def test_contiguous_first_plus_i_materials_on_shifted_handles_fail(self):
        s = json.loads((self.nat / 'state.json').read_text())
        s['materials'] = [[i, h + 1] for i, h in s['materials']]
        (self.nat / 'state.json').write_text(json.dumps(s))
        code, text = self.run_cmp()
        self.assertEqual(code, 1, text)

    def test_whole_vram_known_fails_as_stale(self):
        (self.nat / 'vram.known').write_bytes(b'\x03' * VRAM)
        code, text = self.run_cmp()
        self.assertEqual(code, 1, text)
        self.assertIn('stale_known=3153920', text)

    def test_reference_facts(self):
        ref = json.loads((EVIDENCE / 'reference.json').read_text())
        self.assertEqual([m['selector'] for m in ref['mode8_constructions']], [3, 0, 1])
        self.assertEqual([t['tick'] for t in ref['logo_ticks']], [1, 180, 270, 480, 600])
        self.assertEqual(ref['rand_after_prefix']['calls'], 0)
        self.assertTrue(all(ref['startup_cross_check']['prefix_state'].values()))
        self.assertTrue(ref['rng_cross_check']['prefix_rand_entries_equal'])
        trav = ref['handle_successor_traversal']
        self.assertEqual([t['handle'] for t in trav], list(range(44, 111)))
        self.assertEqual(trav[-1]['successor'], -7)
        self.assertEqual(ref['reserve_class_windows']['handle_operations_with_class_1'], 0)

NATIVE_REPORT = MAIN / 'local/research/title-session-native-v1/release/title-report'

@unittest.skipUnless((EVIDENCE / 'events.jsonl').is_file() and (NATIVE_REPORT / 'state.json').is_file(),
                     'Title native Session title report absent')
class RetainedNativeSession(unittest.TestCase):
    def test_retained_native_title_report_is_identical(self):
        out, err = io.StringIO(), io.StringIO()
        from contextlib import redirect_stderr, redirect_stdout
        with redirect_stdout(out), redirect_stderr(err):
            code = cmp.main([str(EVIDENCE), str(NATIVE_REPORT)])
        self.assertEqual(code, 0, out.getvalue()[-3000:] + err.getvalue())

if __name__ == '__main__':
    unittest.main()
