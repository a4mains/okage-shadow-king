"""Controls for tools/newgame_compare.py (New Game -> name entry -> Chapter 1).

A SYNTHETIC but schema-valid original analysis directory, manifest and native report directory are built
programmatically in a fresh temporary directory per test. The base pair compares IDENTICAL; each negative control
mutates a fresh copy and asserts the exact exit code and, where useful, the reason in the captured output.

Most tests shrink the comparator's VRAM size to 4 KiB (as tests/test_restore_compare.py does) so sixteen
checkpoints of synthetic GS memory stay small; FullSizeVram builds the base pair with the real 4 MiB files (hard-linked
within each side) once.
"""
import hashlib
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import restore_compare as g  # noqa: E402
import newgame_compare as cmp  # noqa: E402

ELF = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
SMALL = 4096
FULL = 1 << 22
CPS = cmp.CHECKPOINTS
S0 = 1000
THEME_END = 300
# reference vblank-independent traversal t of each checkpoint (original); native frame = S0 + t
T = dict(zip(CPS, [-1, 0, 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130]))
SHA_A, SHA_B = 'a' * 64, 'b' * 64

def jl(rows):
    return ''.join(json.dumps(r) + '\n' for r in rows)

def cov(cp):
    return {'lifecycle': 'newgame', 'checkpoint': cp, 'memory': cmp.MEMORY}

# Sixteen lifecycle segments; checkpoint k sees the concatenation of SEG[:k+1].
SEG = [
    [{'kind': 'phase', 'name': 'prefix'}, {'kind': 'mode', 'op': 'request', 'mode': 9, 'selector': 0, 'arg': 0}],
    [{'kind': 'alloc', 'alloc': 200, 'class': 1, 'psm': 19, 'w': 256, 'h': 64, 'tbp': 100},
     {'kind': 'handle', 'handle': 40, 'record': 120, 'alias': False},
     {'kind': 'upload', 'alloc': 200},
     {'kind': 'upload-bytes', 'alloc': 200, 'bytes': 8192, 'sha256': SHA_A},
     {'kind': 'ui', 'op': 'manager-create', 'mode': 9, 'windows': 1},
     {'kind': 'ui', 'op': 'window-create', 'window': 0}],
    [{'kind': 'glyph', 'op': 'cache-transfer'}, {'kind': 'effect', 'op': 'reclaim', 'slot': 0}],
    [{'kind': 'ref', 'alloc': 200, 'delta': 1, 'refs': 2}, {'kind': 'name', 'handle': 40, 'name': 'bm_if'}],
    [{'kind': 'ref', 'alloc': 200, 'delta': -1, 'refs': 1}],
    [{'kind': 'ui', 'op': 'window-death', 'window': 0}, {'kind': 'ui', 'op': 'manager-destroy', 'mode': 9}],
    [{'kind': 'mode', 'op': 'wrapper-free', 'node': 'manager', 'mode': 9, 'selector': 0}],
    [{'kind': 'mode', 'op': 'command', 'command': [14, 1, 0]},
     {'kind': 'mode', 'op': 'request', 'mode': 2, 'selector': 72, 'arg': 0}],
    [{'kind': 'mode', 'op': 'resource-retire', 'mode': 9, 'selector': 0}],
    [{'kind': 'mode', 'op': 'destroy', 'mode': 9, 'selector': 0}, {'kind': 'retire', 'handle': 40},
     {'kind': 'free', 'alloc': 200}, {'kind': 'mode', 'op': 'wrapper-free', 'node': 'resource', 'mode': 9, 'selector': 0}],
    [{'kind': 'alloc', 'alloc': 210, 'class': 1, 'psm': 19, 'w': 128, 'h': 128, 'tbp': 300},
     {'kind': 'handle', 'handle': 41, 'record': 121, 'alias': False},
     {'kind': 'upload', 'alloc': 210},
     {'kind': 'upload-bytes', 'alloc': 210, 'bytes': 4096, 'sha256': SHA_B}],
    [{'kind': 'mode', 'op': 'request', 'mode': 8, 'selector': 1, 'arg': 5}],
    [{'kind': 'mode', 'op': 'command', 'command': [0, 0, 0]}, {'kind': 'effect', 'op': 'remove-request', 'slot': 0}],
    [{'kind': 'mode', 'op': 'resource-retire', 'mode': 8, 'selector': 1}],
    [{'kind': 'mode', 'op': 'destroy', 'mode': 8, 'selector': 1}, {'kind': 'retire', 'handle': 41},
     {'kind': 'free', 'alloc': 210}, {'kind': 'mode', 'op': 'wrapper-free', 'node': 'mode8', 'mode': 8, 'selector': 1}],
    [{'kind': 'mode', 'op': 'wrapper-free', 'node': 'mode2', 'mode': 2, 'selector': 72}],
]
PREFIX = {cp: sum(SEG[:i + 1], []) for i, cp in enumerate(CPS)}
PHASES = ['noise', 'effect-grid', 'effect-template', 'session', 'name-ui-icon', 'name-ui-glyph', 'name-ui-glyph',
          'name-ui-select', 'session', 'name-ui-select', 'name-ui-select', 'session']
RNG = [{'i': i, 'phase': p, 'state': 1000 + i, 'value': 50 + i} for i, p in enumerate(PHASES)]
RNG_AT = dict(zip(CPS, [1, 4, 6, 8, 9, 10, 10, 10, 11, 11, 12, 12, 12, 12, 12, 12]))
GLYPH_IDX = 5

def state(cp):
    return {
        'coverage': cov(cp),
        'allocations': [{'id': 200, 'serial': 1, 'class': 1, 'psm': 19, 'width': 256, 'height': 64, 'tbp': 100, 'tbw': 4,
                         'references': 1, 'descriptor': 999}],
        'handles': [{'handle': 40, 'record': 120, 'image': [200, 1], 'palettes': [None]}],
        'records': [{'record': 120, 'name': '', 'hash_slot': -1, 'handles': 1}],
        'hash': [[3, -1]], 'reserved': [], 'free_records': 511, 'free_extras': 128, 'available': 4095,
        'cursors': [0] * 64, 'free_queue_ids': [1, 2, 3], 'occupancy_hex': '00' * 8192, 'image_list': [],
        'materials': [],
    }

def fx(i):
    return ({'slot': 0, 'shape': 106, 'state': 'reclaim-pending', 'element_handles': [43]} if i < 2 else
            {'slot': 0, 'shape': None, 'state': 'reclaimed', 'element_handles': []})

def session(i):
    name = '\u0000' * 32 if i < 7 else 'Hero' + '\u0000' * 28
    return {'image_counter': i, 'command': [14, 1, 0] if 7 <= i <= 11 else [0, 0, 0], 'name_844': name,
            'temp_10_sha256': 'c' * 64,
            'fade': {'current': '0x0', 'target': '0xff', 'rate': '0x8', 'color': '0x0'},
            'mode_parent': {'phase': 0, 'queue': [0, 0, 0], 'current': [2, 72, 0]}}

def name_rows():
    rows = []
    for t in range(5):
        rows.append({'t': t, 'manager_ran': True, 'phase_in': [0, 1, 1, 2, -3][t], 'phase_out': [1, 1, 2, -3, -3][t],
                     'length_out': t, 'selection_out': 0, 'yes_no_out': 0, 'pressed': [0, 0x10, 0, 0x20, 0][t],
                     'repeat': 0, 'draws': 1 if t == 0 else 0, 'name_out': 'A' * t})
    for t in (5, 6):
        rows.append({'t': t, 'manager_ran': False, 'phase_in': None, 'phase_out': None, 'length_out': None,
                     'selection_out': None, 'yes_no_out': None, 'pressed': None, 'repeat': None, 'draws': 0,
                     'name_out': None})
    return rows

CHAPTER_CALLS = 245

def chapter_rows():
    return [{'t': 20 + i, 'counter_out': i + 1, 'phase_out': 19, 'fade_current_in': i, 'fade_target_in': 255,
             'fade_target_out': 255, 'command_out': [0, 0, 0] if i == CHAPTER_CALLS - 1 else [14, 1, 0],
             'image_counter_in': 3, 'draws': 0} for i in range(CHAPTER_CALLS)]

def count_nibbles(data):
    return sum((b & 1) + ((b >> 1) & 1) for b in data)

class Fixture:
    def __init__(self, tmp, vram=SMALL):
        self.tmp = Path(tmp)
        self.orig = self.tmp / 'analysis'
        self.nat = self.tmp / 'native'
        self.manifest = self.tmp / 'manifest.json'
        self.vram = vram
        mask, vals, inval = bytearray(vram), bytearray(vram), bytearray(vram)
        mask[0x10], vals[0x10] = 3, 0x5a
        mask[0x11], vals[0x11] = 1, 0x07
        inval[0x20] = 3
        mask[0x30], vals[0x30] = 3, 0x99
        data = bytearray(vals)
        data[0x11] = 0xf7
        self.mask, self.vals, self.inval, self.data = bytes(mask), bytes(vals), bytes(inval), bytes(data)
        self.known_n, self.unknown_n = count_nibbles(mask), count_nibbles(inval)
        self.orig.mkdir()
        self.nat.mkdir()
        shared = {}
        for i, cp in enumerate(CPS):
            d, n = self.orig / cp, self.nat / cp
            d.mkdir()
            n.mkdir()
            (d / 'events.jsonl').write_text(jl(PREFIX[cp]))
            (n / 'lifecycle.jsonl').write_text(jl(PREFIX[cp]))
            for side in (d, n):
                (side / 'state.json').write_text(json.dumps(state(cp)))
                (side / 'effects.json').write_text(json.dumps({'effect106': fx(i)}))
                (side / 'rng.jsonl').write_text(jl(RNG[:RNG_AT[cp]]))
                (side / 'session.json').write_text(json.dumps(session(i)))
            self.put(shared, d / 'vram-written-mask.bin', self.mask)
            self.put(shared, d / 'vram-written-values.bin', self.vals)
            self.put(shared, d / 'vram-invalidated-mask.bin', self.inval)
            self.put(shared, n / 'vram.bytes', self.data)
            self.put(shared, n / 'vram.known', self.mask)
            self.put(shared, n / 'vram.invalidated', self.inval)
        (self.orig / 'name-frames.jsonl').write_text(jl(name_rows()))
        (self.orig / 'chapter-frames.jsonl').write_text(jl(chapter_rows()))
        (self.nat / 'name-frames.jsonl').write_text(jl(name_rows()))
        (self.nat / 'chapter-frames.jsonl').write_text(jl(chapter_rows()))
        (self.orig / "reference.json").write_text(json.dumps({
            'checkpoints': {cp: {'t': T[cp]} for cp in CPS}, 'tails': {'theme_end_t': THEME_END},
            'rng': {'provenance': 'every draw is a hooked 0x1f14b0 entry (synthetic fixture)'},
            'problems': [], 'retire_neutrality': {'failures': []}, 'control_minimal': {'failures': []}}))
        (self.nat / 'route.json').write_text(json.dumps(self.route_json()))
        self.pin()

    def put(self, shared, path, data):
        """Write a file; at full size, files of equal content on one side share an inode (hard link)."""
        key = (str(path.parent.parent), path.name)
        if self.vram == FULL and key in shared:
            os.link(shared[key], path)
            return
        path.write_bytes(data)
        shared[key] = path

    def route_json(self):
        return {'route': 'newgame-chapter', 'elf_sha256': ELF, 'construction_frame': S0 + T['name-constructor-return'],
                'checkpoints': {cp: S0 + T[cp] for cp in CPS}, 'tails': {'theme_end_t': THEME_END}}

    def pin(self, elf=ELF, drop=None):
        outs, dig = {}, {}
        for p in sorted(self.orig.rglob('*')):
            if p.is_file():
                b = p.read_bytes()
                outs['analysis/' + str(p.relative_to(self.orig))] = {'sha256': hashlib.sha256(b).hexdigest(), 'bytes': len(b)}
        if drop:
            del outs['analysis/' + drop]
        for i, cp in enumerate(CPS):
            dig[cp] = {'resource_events': sum(1 for e in PREFIX[cp] if e['kind'] != 'phase'), 'rng_draws': RNG_AT[cp],
                       'vram_known_nibbles': self.known_n, 'vram_unknown_nibbles': self.unknown_n, 't': T[cp]}
        nf = [json.loads(x) for x in (self.orig / 'name-frames.jsonl').read_text().splitlines()]
        cf = [json.loads(x) for x in (self.orig / 'chapter-frames.jsonl').read_text().splitlines()]
        anchors = {'checkpoint_t': dict(T), 'name': cmp.name_anchors(nf), 'chapter': cmp.chapter_anchors(cf),
                   'theme_end_t': THEME_END}
        self.manifest.write_text(json.dumps({'inputs': {'elf': {'sha256': elf}}, 'outputs': outs,
                                             'result_digest': {'checkpoints': dig, 'anchors': anchors}}))

    def run(self, native=None, strict=False):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = cmp.main([str(self.orig), str(native or self.nat), '--manifest', str(self.manifest)]
                            + (['--strict'] if strict else []))
        return code, out.getvalue() + err.getvalue()

    # ---- fixture editing helpers (native side unless a path is given)
    def rows(self, path):
        return [json.loads(x) for x in Path(path).read_text().splitlines()]

    def edit_rows(self, path, fn):
        p = Path(path)
        p.write_text(jl(fn(self.rows(p))))

    def edit_json(self, path, fn):
        p = Path(path)
        d = json.loads(p.read_text())
        fn(d)
        p.write_text(json.dumps(d))

    def from_cp(self, first, name, fn, root=None):
        """Apply fn(rows)->rows to <root>/<cp>/<name> for every checkpoint with index >= first."""
        for cp in CPS[first:]:
            self.edit_rows((root or self.nat) / cp / name, fn)

def drop_row(rows, kind, op=None):
    k = next(i for i, r in enumerate(rows) if r.get('kind') == kind and (op is None or r.get('op') == op))
    return rows[:k] + rows[k + 1:]

class NewGameControls(unittest.TestCase):
    VRAM = SMALL

    def setUp(self):
        self.saved = g.VRAM
        g.VRAM = self.VRAM
        self.addCleanup(setattr, g, 'VRAM', self.saved)
        self.td = tempfile.TemporaryDirectory()
        self.addCleanup(self.td.cleanup)
        self.f = Fixture(self.td.name, self.VRAM)

    def expect(self, code, text=None, strict=False):
        c, out = self.f.run(strict=strict)
        self.assertEqual(c, code, out)
        if text:
            self.assertIn(text, out)
        return out

    # ---- the base pair
    def test_base_identical(self):
        out = self.expect(0, 'RESULT: IDENTICAL')
        self.assertIn('owner-asserted rows lifecycle=0 rng=0 frames=0', out)
        self.assertIn('anchors: identical', out)
        self.assertIn('name frames: identical (7 rows)', out)
        self.assertIn(f'chapter frames: identical ({CHAPTER_CALLS} rows)', out)
        self.assertEqual(out.count('events: identical'), 16)
        self.assertNotIn('DIFFERENT', out)

    def test_base_json_report(self):
        path = Path(self.td.name) / 'result.json'
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            code = cmp.main([str(self.f.orig), str(self.f.nat), '--manifest', str(self.f.manifest), '--json', str(path)])
        self.assertEqual(code, 0, out.getvalue())
        r = json.loads(path.read_text())
        self.assertEqual(r['result'], 'IDENTICAL')
        self.assertFalse(r['anchors_differ'])
        self.assertTrue(all(c['identical'] for c in r['checkpoints'].values()))
        self.assertEqual(r['frames_coverage'], {'name': [], 'chapter': []})

    def test_phase_rows_are_informational(self):
        self.f.from_cp(0, 'lifecycle.jsonl', lambda rows: rows + [{'kind': 'phase', 'name': 'extra'}])
        self.expect(0, 'RESULT: IDENTICAL')

    # ---- lifecycle
    def test_missing_lifecycle_event(self):
        self.f.from_cp(1, 'lifecycle.jsonl', lambda rows: drop_row(rows, 'upload'))
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('events: DIFFERENT at event', out)
        self.assertNotIn('stream_inconsistent', out)

    def test_reordered_events(self):
        def swap(rows):
            k = next(i for i, r in enumerate(rows) if r['kind'] == 'alloc')
            rows[k], rows[k + 1] = rows[k + 1], rows[k]
            return rows
        self.f.from_cp(1, 'lifecycle.jsonl', swap)
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('events: DIFFERENT at event', out)
        self.assertNotIn('stream_inconsistent', out)

    def test_extra_lifecycle_event(self):
        # an invented glyph transfer after the segment-2 events, kept in every later checkpoint
        def add(rows):
            k = next(i for i, r in enumerate(rows) if r.get('op') == 'cache-transfer')
            return rows[:k] + [{'kind': 'glyph', 'op': 'cache-transfer'}] + rows[k:]
        self.f.from_cp(2, 'lifecycle.jsonl', add)
        self.expect(1, 'events: DIFFERENT')

    def test_asserted_lifecycle_row_is_unproven(self):
        def mark(rows):
            k = next(i for i, r in enumerate(rows) if r['kind'] == 'name')
            rows[k]['asserted'] = True
            return rows
        self.f.from_cp(3, 'lifecycle.jsonl', mark)
        out = self.expect(4, 'RESULT: UNPROVEN')
        self.assertIn('owner-asserted rows were needed', out)
        self.assertIn('owner-asserted rows lifecycle=13', out)  # checkpoints 3..15

    def test_asserted_false_is_malformed(self):
        def mark(rows):
            rows[0]['asserted'] = False
            return rows
        self.f.edit_rows(self.f.nat / CPS[0] / 'lifecycle.jsonl', mark)
        self.expect(2, '"asserted" must be true')

    def test_asserted_rng_and_frame_rows_are_unproven(self):
        def mark(rows):
            rows[0]['asserted'] = True
            return rows
        self.f.from_cp(0, 'rng.jsonl', mark)
        out = self.expect(4, 'RESULT: UNPROVEN')
        self.assertIn('rng=16', out)
        self.f.from_cp(0, 'rng.jsonl', lambda rows: [{k: v for k, v in r.items() if k != 'asserted'} for r in rows])
        self.expect(0)
        self.f.edit_rows(self.f.nat / 'name-frames.jsonl', mark)
        out = self.expect(4, 'RESULT: UNPROVEN')
        self.assertIn('frames=1', out)

    def test_original_rows_never_asserted(self):
        self.f.edit_rows(self.f.orig / CPS[0] / 'events.jsonl', lambda rows: [dict(rows[0], asserted=True)] + rows[1:])
        self.f.pin()
        self.expect(2, 'never carry "asserted"')

    # ---- frame coverage
    def test_common_name_row_omission_refuses_original(self):
        for root in (self.f.orig, self.f.nat):
            self.f.edit_rows(root / 'name-frames.jsonl', lambda rows: [r for r in rows if r['t'] != 2])
        self.f.pin()
        out = self.expect(2, 'original frames are incomplete')
        self.assertIn('missing traversals [2]', out)
        self.assertNotIn('RESULT:', out)

    def test_common_chapter_row_omission_refuses_original(self):
        for root in (self.f.orig, self.f.nat):
            self.f.edit_rows(root / 'chapter-frames.jsonl', lambda rows: rows[:100] + rows[101:])
        self.f.pin()
        out = self.expect(2, 'original frames are incomplete')
        self.assertIn('chapter counters are not 1..N', out)

    def test_native_name_row_omission_is_incomplete(self):
        self.f.edit_rows(self.f.nat / 'name-frames.jsonl', lambda rows: [r for r in rows if r['t'] != 2])
        out = self.expect(5, 'RESULT: INCOMPLETE')
        self.assertIn('name frames: INCOMPLETE native coverage', out)
        self.assertIn('missing traversals [2]', out)

    def test_native_name_tail_omission_is_incomplete(self):
        self.f.edit_rows(self.f.nat / 'name-frames.jsonl', lambda rows: rows[:-1])  # the publication traversal
        out = self.expect(5, 'RESULT: INCOMPLETE')
        self.assertIn('expected exactly two manager-less rows', out)

    def test_native_chapter_row_omission_is_incomplete(self):
        self.f.edit_rows(self.f.nat / 'chapter-frames.jsonl', lambda rows: rows[:100] + rows[101:])
        out = self.expect(5, 'RESULT: INCOMPLETE')
        self.assertIn('chapter frames: INCOMPLETE native coverage', out)
        self.assertIn('missing traversals [120]', out)

    def test_native_chapter_tail_omission_is_incomplete(self):
        self.f.edit_rows(self.f.nat / 'chapter-frames.jsonl', lambda rows: rows[:-1])
        out = self.expect(5, 'RESULT: INCOMPLETE')
        self.assertIn('command must be cleared in the last chapter call only', out)

    def test_native_frame_payload_difference(self):
        def bump(rows):
            rows[3]['length_out'] += 1
            return rows
        self.f.edit_rows(self.f.nat / 'name-frames.jsonl', bump)
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('name frames: DIFFERENT at row 3', out)

    # ---- session
    def test_published_name_corruption(self):
        self.f.edit_json(self.f.nat / 'command-published' / 'session.json',
                         lambda d: d.update(name_844='Hera' + '\u0000' * 28))
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('session.name_844: DIFFERENT', out)

    def test_command_corruption_session(self):
        self.f.edit_json(self.f.nat / 'command-published' / 'session.json', lambda d: d.update(command=[14, 2, 0]))
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('session.command: DIFFERENT', out)

    def test_command_corruption_lifecycle_row(self):
        def change(rows):
            k = next(i for i, r in enumerate(rows) if r.get('op') == 'command')
            rows[k]['command'] = [14, 2, 0]
            return rows
        self.f.from_cp(7, 'lifecycle.jsonl', change)
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('events: DIFFERENT at event', out)
        self.assertIn('"command":[14,2,0]', out)

    def _mutate_reference(self, fn):
        ref = json.loads((self.f.orig / "reference.json").read_text())
        fn(ref)
        (self.f.orig / "reference.json").write_text(json.dumps(ref))
        self.f.pin()

    def test_reference_with_problems_is_refused(self):
        self._mutate_reference(lambda r: r.update(problems=['determinism']))
        self.expect(2, 'reference.json reports problems')

    def test_reference_without_control_is_refused(self):
        self._mutate_reference(lambda r: r.pop('control_minimal'))
        self.expect(2, 'reference.json reports problems')

    def test_reference_with_derived_rng_is_refused(self):
        self._mutate_reference(lambda r: r['rng'].update(provenance='LCG-derived between anchors'))
        self.expect(2, 'not all hooked')

    def test_native_invalidated_mask_required(self):
        (self.f.nat / 'first-layout' / 'vram.invalidated').unlink()
        self.expect(2, 'vram.invalidated missing')

    def test_wrong_image_counter(self):
        self.f.edit_json(self.f.nat / 'chapter-constructor-return' / 'session.json', lambda d: d.update(image_counter=99))
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('session.image_counter: DIFFERENT', out)

    def test_wrong_fade(self):
        self.f.edit_json(self.f.nat / 'chapter-fade-request' / 'session.json', lambda d: d['fade'].update(target='0x80'))
        self.expect(1, 'session.fade: DIFFERENT')

    # ---- RNG
    def test_skipped_null_glyph_draw(self):
        def skip(rows):
            rows = [r for r in rows if not (r['i'] == GLYPH_IDX and r['phase'] == 'name-ui-glyph')]
            return [dict(r, i=n) for n, r in enumerate(rows)]
        self.f.from_cp(2, 'rng.jsonl', skip)  # every checkpoint that contains the draw
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('rng: DIFFERENT at draw', out)
        self.assertNotIn('stream_inconsistent', out)

    def test_rng_value_difference(self):
        def change(rows):
            rows[2]['value'] += 1
            return rows
        self.f.edit_rows(self.f.nat / 'first-layout' / 'rng.jsonl', change)
        self.expect(1, 'rng: DIFFERENT at draw 2')

    # ---- VRAM / uploads
    def test_upload_bytes_hash_corruption(self):
        def change(rows):
            for r in rows:
                if r['kind'] == 'upload-bytes' and r['alloc'] == 200:
                    r['sha256'] = 'd' * 64
            return rows
        self.f.from_cp(1, 'lifecycle.jsonl', change)
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('events: DIFFERENT at event', out)
        self.assertIn('d' * 64, out)

    def test_upload_value_mismatch(self):
        def flip(p):
            data = bytearray(p.read_bytes())
            data[0x10] ^= 0x01
            p.write_bytes(bytes(data))
        p = self.f.nat / 'name-edited' / 'vram.bytes'
        if p.stat().st_nlink > 1:
            p.unlink()
            p.write_bytes(self.f.data)
        flip(p)
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('value_mismatch=1', out)

    def test_stale_known_nibble(self):
        p = self.f.nat / 'name-confirmed' / 'vram.known'
        data = bytearray(p.read_bytes())
        data[0x20] = 1
        p.unlink()
        p.write_bytes(bytes(data))
        q = self.f.nat / 'name-confirmed' / 'vram.invalidated'
        inval = bytearray(q.read_bytes())
        inval[0x20] &= ~1
        q.unlink()
        q.write_bytes(bytes(inval))
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('stale_known=1', out)

    def test_missing_knowledge(self):
        p = self.f.nat / 'name-confirmed' / 'vram.known'
        data = bytearray(p.read_bytes())
        data[0x30] = 1
        p.unlink()
        p.write_bytes(bytes(data))
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('missing_knowledge=1', out)

    def test_invented_knowledge(self):
        p = self.f.nat / 'first-layout' / 'vram.known'
        data = bytearray(p.read_bytes())
        data[0x40] = 3
        p.unlink()
        p.write_bytes(bytes(data))
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('invented_knowledge=2', out)

    def test_vram_coverage_gap_allowed_unless_strict(self):
        k = self.f.nat / 'first-layout' / 'vram.known'
        data = bytearray(k.read_bytes())
        data[0x30] = 0
        k.unlink()
        k.write_bytes(bytes(data))
        inv = bytearray(self.f.vram)
        inv[0x30] = 3
        (self.f.nat / 'first-layout' / 'vram.invalidated').write_bytes(bytes(inv))
        out = self.expect(0, 'RESULT: IDENTICAL')
        self.assertIn('coverage_gap_invalidated=2', out)
        self.expect(1, 'strict: 2 coverage gaps are not accepted', strict=True)

    # ---- anchors
    def test_anchor_shift_is_inconclusive(self):
        self.f.edit_json(self.f.nat / 'route.json',
                         lambda d: d['checkpoints'].update({'chapter-fade-request': S0 + T['chapter-fade-request'] + 1}))
        out = self.expect(3, 'RESULT: ANCHORS-DIFFER')
        self.assertIn('INCONCLUSIVE', out)
        self.assertIn('anchors: DIFFERENT', out)
        self.assertIn('anchor checkpoint_t', out)
        self.assertNotIn('RESULT: IDENTICAL', out)

    def test_theme_end_shift_is_anchors_differ(self):
        def shift(d):
            d['tails']['theme_end_t'] = THEME_END + 1
        self.f.edit_json(self.f.nat / 'route.json', shift)
        out = self.expect(3, 'RESULT: ANCHORS-DIFFER')
        self.assertIn('anchor theme_end_t', out)

    def test_press_traversal_shift_is_anchors_differ(self):
        def later(rows):
            rows[1]['pressed'], rows[2]['pressed'] = 0, 0x10  # the press moves one traversal later
            return rows
        self.f.edit_rows(self.f.nat / 'name-frames.jsonl', later)
        out = self.expect(3, 'RESULT: ANCHORS-DIFFER')
        self.assertIn('anchor name', out)
        self.assertIn('INCONCLUSIVE', out)

    def test_anchors_differ_does_not_hide_incomplete(self):
        self.f.edit_json(self.f.nat / 'route.json',
                         lambda d: d['checkpoints'].update({'chapter-fade-request': S0 + T['chapter-fade-request'] + 1}))
        self.f.edit_rows(self.f.nat / 'name-frames.jsonl', lambda rows: [r for r in rows if r['t'] != 2])
        self.expect(5, 'RESULT: INCOMPLETE')

    # ---- stream consistency
    def test_native_checkpoint_stream_not_extending(self):
        self.f.edit_rows(self.f.nat / 'first-layout' / 'lifecycle.jsonl', lambda rows: drop_row(rows, 'upload'))
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('stream_inconsistent', out)

    # ---- identity, pins, self-comparison
    def test_self_comparison_same_directory(self):
        c, out = self.f.run(native=self.f.orig)
        self.assertEqual(c, 2, out)
        self.assertIn('refusing to compare the original analysis with itself', out)

    def test_self_comparison_nested_directory(self):
        nested = self.f.orig / 'native'
        nested.mkdir()
        c, out = self.f.run(native=nested)
        self.assertEqual(c, 2, out)
        self.assertIn('refusing to compare', out)

    def test_self_comparison_shared_file(self):
        p = self.f.nat / 'first-layout' / 'state.json'
        p.unlink()
        os.link(self.f.orig / 'first-layout' / 'state.json', p)
        c, out = self.f.run()
        self.assertEqual(c, 2, out)
        self.assertIn('refusing self-comparison', out)

    def test_modified_original_file_is_refused(self):
        p = self.f.orig / 'name-death' / 'rng.jsonl'
        p.write_text(p.read_text() + jl([{'i': RNG_AT['name-death'], 'phase': 'session', 'state': 1, 'value': 1}]))
        out = self.expect(2, 'does not match the pinned original capture')
        self.assertIn('name-death/rng.jsonl', out)

    def test_modified_original_frames_file_is_refused(self):
        p = self.f.orig / 'chapter-frames.jsonl'
        p.write_text(p.read_text().replace('"image_counter_in": 3', '"image_counter_in": 4', 1))
        self.expect(2, 'analysis/chapter-frames.jsonl does not match the pinned')

    def test_unpinned_original_file_is_refused(self):
        self.f.pin(drop='first-layout/session.json')
        self.expect(2, 'manifest does not pin analysis/first-layout/session.json')

    def test_digest_mismatch_is_refused(self):
        m = json.loads(self.f.manifest.read_text())
        m['result_digest']['checkpoints']['name-edited']['rng_draws'] += 1
        self.f.manifest.write_text(json.dumps(m))
        self.expect(2, 'manifest result digest does not match the original name-edited')

    def test_wrong_elf_sha256_in_route(self):
        self.f.edit_json(self.f.nat / 'route.json', lambda d: d.update(elf_sha256='0' * 64))
        self.expect(2, 'ELF identity differs')

    def test_wrong_manifest_elf(self):
        self.f.pin(elf='1' * 64)
        self.expect(2, 'ELF identity differs')

    def test_route_checkpoint_frames_decreasing(self):
        self.f.edit_json(self.f.nat / 'route.json', lambda d: d['checkpoints'].update({'name-death': S0 + 1}))
        self.expect(2, 'decrease')

    def test_route_construction_frame_must_match(self):
        self.f.edit_json(self.f.nat / 'route.json', lambda d: d.update(construction_frame=S0 + 1))
        self.expect(2, 'construction_frame must be the name-constructor-return frame')

    def test_missing_native_checkpoint_directory(self):
        import shutil
        shutil.rmtree(self.f.nat / 'field-constructor-entry')
        self.expect(2, 'native checkpoint directory field-constructor-entry missing')

    def test_upload_bytes_bad_hash_malformed(self):
        def bad(rows):
            for r in rows:
                if r['kind'] == 'upload-bytes':
                    r['sha256'] = 'XYZ'
            return rows
        self.f.edit_rows(self.f.nat / 'name-constructor-return' / 'lifecycle.jsonl', bad)
        self.expect(2, 'not 64 lower-case hex digits')

    def test_native_session_missing_key_malformed(self):
        self.f.edit_json(self.f.nat / 'first-layout' / 'session.json', lambda d: d.pop('fade'))
        self.expect(2, 'session keys')

class FullSizeVram(NewGameControls):
    """The same base pair with the real 4 MiB GS memory files (hard-linked within each side), plus a few mutations
    that the small-size run also covers. Only the base IDENTICAL pair is repeated at full size (about 13 s)."""
    VRAM = FULL

    def test_base_identical(self):
        self.assertEqual(self.f.vram, FULL)
        super().test_base_identical()

def _keep_full_size_subset():
    keep = {'test_base_identical'}
    for name in dir(FullSizeVram):
        if name.startswith('test_') and name not in keep:
            setattr(FullSizeVram, name, None)

_keep_full_size_subset()

if __name__ == '__main__':
    unittest.main()
