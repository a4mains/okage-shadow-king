"""Controls for tools/restore_compare.py and tools/restore_original_extract.py.

1. SyntheticControls: a small hand-written original/native pair (SYNTHETIC; the comparator's VRAM size is reduced to
   4 KiB for these tests only) exercising every comparator rule and the negative controls required by the review findings (missing/reordered events and checkpoints, changed atlas/glyph bytes, RNG
   draw omission, stale knownness, duplicate/corrupt manifest, common missing stream rows, self/identity mismatch,
   owner-asserted versus observed rows).
2. OriginalDerivedControls: a SYNTHETIC native-like fixture built from the pinned Restore ORIGINAL extract (it is not
   native output) that must compare IDENTICAL and must fail under corruption. Runs when the ignored evidence is
   present (set OKAGE_MAIN_CHECKOUT to the main checkout from a worktree).
3. Extractor unit checks on synthetic oracle events.
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
import restore_compare as cmp  # noqa: E402
import restore_original_extract as ext  # noqa: E402

MAIN = Path(os.environ.get('OKAGE_MAIN_CHECKOUT', ROOT))
EVIDENCE = MAIN / 'local/research/restore-original-v1/analysis'
ELF = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
SMALL = 4096
CPS = cmp.CHECKPOINTS
ATLAS_SHA = 'a' * 64

def jl(rows):
    return ''.join(json.dumps(r) + '\n' for r in rows)

def cov(cp):
    return {'lifecycle': 'restore', 'checkpoint': cp, 'memory': cmp.MEMORY}

# Eight-checkpoint synthetic stream: each checkpoint sees the concatenation of STREAM[:k+1].
STREAM = [
    [{'kind': 'phase', 'name': 'prefix-end-0x100618'},
     {'kind': 'mode', 'op': 'request', 'mode': 10, 'selector': 0, 'arg': 0}],
    [{'kind': 'alloc', 'alloc': 166, 'class': 1, 'psm': 19, 'w': 256, 'h': 320, 'tbp': 100},
     {'kind': 'handle', 'handle': 36, 'record': 113, 'alias': False},
     {'kind': 'upload', 'alloc': 166},
     {'kind': 'upload-bytes', 'alloc': 166, 'bytes': 81920, 'sha256': ATLAS_SHA},
     {'kind': 'ui', 'op': 'manager-create', 'mode': 10, 'windows': 1},
     {'kind': 'ui', 'op': 'window-create', 'window': 0}],
    [{'kind': 'effect', 'op': 'reclaim', 'slot': 0}],
    [{'kind': 'ui', 'op': 'state', 'state': 510}, {'kind': 'ui', 'op': 'state', 'state': 540}],
    [{'kind': 'ui', 'op': 'state', 'state': -1}, {'kind': 'ui', 'op': 'state', 'state': 0}],
    [{'kind': 'ui', 'op': 'manager-destroy', 'mode': 10}, {'kind': 'ui', 'op': 'window-death', 'window': 0},
     {'kind': 'mode', 'op': 'wrapper-free', 'node': 'manager', 'mode': 10, 'selector': 0},
     {'kind': 'mode', 'op': 'request', 'mode': 8, 'selector': 1, 'arg': 0},
     {'kind': 'mode', 'op': 'resource-retire', 'mode': 10, 'selector': 0}],
    [{'kind': 'mode', 'op': 'destroy', 'mode': 10, 'selector': 0}, {'kind': 'retire', 'handle': 36},
     {'kind': 'free', 'alloc': 166}, {'kind': 'mode', 'op': 'wrapper-free', 'node': 'resource', 'mode': 10, 'selector': 0}],
    [{'kind': 'mode', 'op': 'wrapper-free', 'node': 'mode10', 'mode': 10, 'selector': 0},
     {'kind': 'alloc', 'alloc': 168, 'class': 1, 'psm': 19, 'w': 64, 'h': 64, 'tbp': 100},
     {'kind': 'handle', 'handle': 36, 'record': 114, 'alias': False}],
]
PREFIX = {cp: sum(STREAM[:i + 1], []) for i, cp in enumerate(CPS)}
RNG = [{'i': 0, 'phase': 'noise', 'state': 12345, 'value': 12345},
       {'i': 1, 'phase': 'restore-ui-icon', 'state': 7, 'value': 7},
       {'i': 2, 'phase': 'restore-ui-glyph', 'state': 9, 'value': 9}]
RNG_AT = {cp: (1 if i == 0 else 2 if i < 3 else 3) for i, cp in enumerate(CPS)}
FX = {cp: ({'slot': 0, 'shape': 106, 'state': 'reclaim-pending', 'element_handles': [43]} if i < 2 else
           {'slot': 0, 'shape': None, 'state': 'reclaimed', 'element_handles': []}) for i, cp in enumerate(CPS)}
FRAMES = [
    {'t': 0, 'state_in': 0, 'state_out': 510, 'update_state_out': 510, 'gate_open': False, 'pressed': None, 'draws': 1},
    {'t': 1, 'state_in': 510, 'state_out': 510, 'update_state_out': 540, 'gate_open': False, 'pressed': None, 'draws': 1},
    {'t': 2, 'state_in': 540, 'state_out': 540, 'update_state_out': 540, 'gate_open': True, 'pressed': 0, 'draws': 0},
    {'t': 3, 'state_in': 540, 'state_out': -1, 'update_state_out': -1, 'gate_open': True, 'pressed': 0x20, 'draws': 0},
    {'t': 4, 'state_in': -1, 'state_out': -1, 'update_state_out': -1, 'gate_open': False, 'pressed': None, 'draws': 0},
    {'t': 5, 'state_in': -1, 'state_out': 0, 'update_state_out': 0, 'gate_open': False, 'pressed': None, 'draws': 0},
]

def state(cp):
    live = cp not in ('wrapper-free',)
    return {
        'coverage': cov(cp),
        'allocations': [{'id': 166, 'serial': 1, 'class': 1, 'psm': 19, 'width': 256, 'height': 320, 'tbp': 100, 'tbw': 4,
                         'references': 1, 'descriptor': 999}] if live else [],
        'handles': [{'handle': 36, 'record': 113, 'image': [166, 1], 'palettes': [None]}] if live else [],
        'records': [{'record': 113, 'name': '', 'hash_slot': -1, 'handles': 1}] if live else [],
        'hash': [[3, -1]], 'reserved': [], 'free_records': 511, 'free_extras': 128,
        'available': 4095 if live else 4096, 'cursors': [0] * 64, 'free_queue_ids': [1, 2, 3],
        'occupancy_hex': '00' * 8192, 'image_list': [], 'materials': [],
    }

class Fixture:
    def __init__(self, tmp):
        self.tmp = Path(tmp)
        self.orig = self.tmp / 'analysis'
        self.nat = self.tmp / 'native'
        self.manifest = self.tmp / 'manifest.json'
        mask, vals, inval = bytearray(SMALL), bytearray(SMALL), bytearray(SMALL)
        mask[0x10], vals[0x10] = 3, 0x5a
        mask[0x11], vals[0x11] = 1, 0x07
        inval[0x20] = 3
        mask[0x30], vals[0x30] = 3, 0x99
        self.mask, self.vals, self.inval = mask, vals, inval
        for cp in CPS:
            d = self.orig / cp
            d.mkdir(parents=True)
            (d / 'events.jsonl').write_text(jl(PREFIX[cp]))
            (d / 'state.json').write_text(json.dumps(state(cp)))
            (d / 'effects.json').write_text(json.dumps({'effect106': FX[cp]}))
            (d / 'rng.jsonl').write_text(jl(RNG[:RNG_AT[cp]]))
            (d / 'vram-written-mask.bin').write_bytes(bytes(mask))
            (d / 'vram-written-values.bin').write_bytes(bytes(vals))
            (d / 'vram-invalidated-mask.bin').write_bytes(bytes(inval))
            n = self.nat / cp
            n.mkdir(parents=True)
            (n / 'lifecycle.jsonl').write_text(jl(PREFIX[cp]))
            (n / 'state.json').write_text(json.dumps(state(cp)))
            (n / 'effects.json').write_text(json.dumps({'effect106': FX[cp]}))
            (n / 'rng.jsonl').write_text(jl(RNG[:RNG_AT[cp]]))
            data = bytearray(vals)
            data[0x11] = 0xf7
            (n / 'vram.bytes').write_bytes(bytes(data))
            (n / 'vram.known').write_bytes(bytes(mask))
        (self.orig / 'manager-frames.jsonl').write_text(jl([dict(r, vb=1000 + r['t'], opacity_bits=None, repeat=0)
                                                            for r in FRAMES]))
        (self.nat / 'manager-frames.jsonl').write_text(jl(FRAMES))
        (self.nat / 'route.json').write_text(json.dumps(self.route_json()))
        self.pin()

    def route_json(self, **kw):
        frames = [100, 105, 105, 110, 120, 125, 128, 200]
        r = {'route': 'restore-cancel', 'elf_sha256': ELF, 'checkpoints': dict(zip(CPS, frames))}
        r.update(kw)
        return r

    def pin(self, elf=ELF, frames=None):
        outs, dig = {}, {}
        for p in sorted(self.orig.rglob('*')):
            if p.is_file():
                b = p.read_bytes()
                outs['analysis/' + str(p.relative_to(self.orig))] = {'sha256': hashlib.sha256(b).hexdigest(), 'bytes': len(b)}
        for cp in CPS:
            dig[cp] = {'resource_events': sum(1 for e in PREFIX[cp] if e['kind'] != 'phase'), 'rng_draws': RNG_AT[cp],
                       'vram_known_nibbles': 5, 'vram_unknown_nibbles': 2}
        rows = [json.loads(x) for x in (self.orig / 'manager-frames.jsonl').read_text().splitlines()]
        mf = cmp.anchors([{k: r[k] for k in cmp.FRAME_KEYS} for r in rows]) if frames is None else frames
        self.manifest.write_text(json.dumps({'inputs': {'elf': {'sha256': elf}}, 'outputs': outs,
                                             'result_digest': {'checkpoints': dig, 'manager_frames': mf}}))

    def run(self, strict=False):
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = cmp.main([str(self.orig), str(self.nat), '--manifest', str(self.manifest)] + (['--strict'] if strict else []))
        return code, out.getvalue() + err.getvalue()

    def rewrite(self, cp, name, rows):
        (self.nat / cp / name).write_text(jl(rows))

    def native_rows(self, cp, name='lifecycle.jsonl'):
        return [json.loads(x) for x in (self.nat / cp / name).read_text().splitlines()]

    def set_frames(self, rows, original=False):
        if original:
            (self.orig / 'manager-frames.jsonl').write_text(jl(rows))
        else:
            (self.nat / 'manager-frames.jsonl').write_text(jl(rows))

class SyntheticControls(unittest.TestCase):
    def setUp(self):
        self.saved = cmp.VRAM
        cmp.VRAM = SMALL
        self.td = tempfile.TemporaryDirectory()
        self.f = Fixture(self.td.name)

    def tearDown(self):
        cmp.VRAM = self.saved
        self.td.cleanup()

    def expect(self, code, text=None, strict=False):
        c, out = self.f.run(strict)
        self.assertEqual(c, code, out)
        if text:
            self.assertIn(text, out)
        return out

    def every_cp(self, fn):
        for cp in CPS:
            fn(cp)

    def test_identical(self):
        out = self.expect(0, 'RESULT: IDENTICAL')
        self.assertIn('owner-asserted rows lifecycle=0', out)

    def test_phase_rows_informational(self):
        self.every_cp(lambda cp: self.f.rewrite(cp, 'lifecycle.jsonl', self.f.native_rows(cp) + [{'kind': 'phase', 'name': 'x'}]))
        self.expect(0)

    def test_missing_event(self):
        for i, cp in enumerate(CPS):
            if i >= 1:
                rows = [r for r in self.f.native_rows(cp) if r.get('kind') != 'upload']
                self.f.rewrite(cp, 'lifecycle.jsonl', rows)
        self.expect(1, 'events: DIFFERENT')

    def test_reordered_events(self):
        for i, cp in enumerate(CPS):
            if i >= 6:
                rows = self.f.native_rows(cp)
                k = next(j for j, r in enumerate(rows) if r.get('op') == 'destroy' and r.get('mode') == 10)
                rows[k], rows[k + 1] = rows[k + 1], rows[k]   # retire before the wrapper destructor
                self.f.rewrite(cp, 'lifecycle.jsonl', rows)
        self.expect(1, 'events: DIFFERENT')

    def test_changed_atlas_bytes(self):
        for i, cp in enumerate(CPS):
            if i >= 1:
                rows = self.f.native_rows(cp)
                for r in rows:
                    if r['kind'] == 'upload-bytes':
                        r['sha256'] = 'b' * 64
                self.f.rewrite(cp, 'lifecycle.jsonl', rows)
        self.expect(1, 'events: DIFFERENT')

    def test_invented_glyph_write(self):
        for i, cp in enumerate(CPS):
            if i >= 3:
                rows = self.f.native_rows(cp)
                k = next(j for j, r in enumerate(rows) if r.get('op') == 'state')
                rows.insert(k, {'kind': 'glyph', 'op': 'cache-transfer'})
                self.f.rewrite(cp, 'lifecycle.jsonl', rows)
        self.expect(1, 'events: DIFFERENT')

    def test_malformed_upload_bytes_hash(self):
        rows = self.f.native_rows('restore-constructor-return')
        for r in rows:
            if r['kind'] == 'upload-bytes':
                r['sha256'] = 'XYZ'
        self.f.rewrite('restore-constructor-return', 'lifecycle.jsonl', rows)
        self.expect(2)

    def test_unknown_node_and_extra_key(self):
        rows = self.f.native_rows('wrapper-free') + [{'kind': 'mode', 'op': 'wrapper-free', 'node': 'other', 'mode': 10, 'selector': 0}]
        self.f.rewrite('wrapper-free', 'lifecycle.jsonl', rows)
        self.expect(2)

    def test_bool_as_int(self):
        rows = self.f.native_rows('empty-ready')
        rows[-1]['state'] = True
        self.f.rewrite('empty-ready', 'lifecycle.jsonl', rows)
        self.expect(2)

    def test_asserted_rows_are_unproven(self):
        for i, cp in enumerate(CPS):
            rows = self.f.native_rows(cp)
            for r in rows:
                if r['kind'] == 'ui' and r['op'] == 'window-death':
                    r['asserted'] = True
            self.f.rewrite(cp, 'lifecycle.jsonl', rows)
        out = self.expect(4, 'RESULT: UNPROVEN')
        self.assertIn('owner-asserted', out)

    def test_asserted_rng_rows_are_unproven(self):
        for cp in CPS:
            rows = self.f.native_rows(cp, 'rng.jsonl')
            for r in rows:
                if r['phase'] != 'noise':
                    r['asserted'] = True
            self.f.rewrite(cp, 'rng.jsonl', rows)
        self.expect(4, 'RESULT: UNPROVEN')

    def test_asserted_false_is_malformed(self):
        rows = self.f.native_rows('empty-ready')
        rows[-1]['asserted'] = False
        self.f.rewrite('empty-ready', 'lifecycle.jsonl', rows)
        self.expect(2)

    def test_original_never_asserted(self):
        p = self.f.orig / 'empty-ready' / 'events.jsonl'
        rows = [json.loads(x) for x in p.read_text().splitlines()]
        rows[-1]['asserted'] = True
        p.write_text(jl(rows))
        self.f.pin()
        self.expect(2)

    def test_asserted_row_cannot_hide_difference(self):
        for i, cp in enumerate(CPS):
            if i >= 5:
                rows = self.f.native_rows(cp)
                for r in rows:
                    if r.get('op') == 'window-death':
                        r['window'] = 3
                        r['asserted'] = True
                self.f.rewrite(cp, 'lifecycle.jsonl', rows)
        self.expect(1, 'DIFFERENT')

    def test_missing_checkpoint(self):
        shutil.rmtree(self.f.nat / 'first-layout')
        self.expect(2)

    def test_reordered_checkpoints(self):
        r = self.f.route_json()
        r['checkpoints']['empty-ready'], r['checkpoints']['cancel-death'] = 120, 110
        (self.f.nat / 'route.json').write_text(json.dumps(r))
        self.expect(2)

    def test_duplicated_checkpoint_content(self):
        # cancel-death reported with the empty-ready stream: lifecycle does not strictly extend
        self.f.rewrite('cancel-death', 'lifecycle.jsonl', PREFIX['empty-ready'])
        self.expect(1, 'stream_inconsistent')

    def test_rng_draw_omitted(self):
        for i, cp in enumerate(CPS):
            if RNG_AT[cp] == 3:
                self.f.rewrite(cp, 'rng.jsonl', RNG[:2])
        self.expect(1, 'rng: DIFFERENT')

    def test_rng_value_changed(self):
        for cp in CPS:
            rows = self.f.native_rows(cp, 'rng.jsonl')
            if len(rows) > 1:
                rows[1]['value'] = 8
            self.f.rewrite(cp, 'rng.jsonl', rows)
        self.expect(1, 'rng: DIFFERENT')

    def test_rng_unknown_phase(self):
        rows = self.f.native_rows('second-title-return', 'rng.jsonl')
        rows[-1]['phase'] = 'restore-whatever'
        self.f.rewrite('second-title-return', 'rng.jsonl', rows)
        self.expect(2)

    def test_stale_known(self):
        k = bytearray((self.f.nat / 'cancel-death' / 'vram.known').read_bytes())
        k[0x20] = 3
        (self.f.nat / 'cancel-death' / 'vram.known').write_bytes(bytes(k))
        self.expect(1, 'stale_known=2')

    def test_value_mismatch_and_missing_knowledge(self):
        b = bytearray((self.f.nat / 'first-layout' / 'vram.bytes').read_bytes())
        b[0x10] = 0x5b
        (self.f.nat / 'first-layout' / 'vram.bytes').write_bytes(bytes(b))
        k = bytearray((self.f.nat / 'wrapper-free' / 'vram.known').read_bytes())
        k[0x30] = 0
        (self.f.nat / 'wrapper-free' / 'vram.known').write_bytes(bytes(k))
        out = self.expect(1)
        self.assertIn('value_mismatch=1', out)
        self.assertIn('missing_knowledge=2', out)

    def test_state_difference(self):
        s = state('resource-retirement')
        s['available'] = 1
        (self.f.nat / 'resource-retirement' / 'state.json').write_text(json.dumps(s))
        self.expect(1, 'state.available: DIFFERENT')

    def test_state_wrong_coverage(self):
        s = state('empty-ready')
        s['coverage']['lifecycle'] = 'retire-title'
        (self.f.nat / 'empty-ready' / 'state.json').write_text(json.dumps(s))
        self.expect(2)

    def test_effect_difference(self):
        (self.f.nat / 'first-layout' / 'effects.json').write_text(json.dumps({'effect106': FX['retire-successor-entry']}))
        self.expect(1, 'effect106: DIFFERENT')

    def test_duplicate_json_key(self):
        (self.f.nat / 'empty-ready' / 'effects.json').write_text('{"effect106": null, "effect106": null}')
        self.expect(2)

    def test_truncated_lifecycle(self):
        p = self.f.nat / 'empty-ready' / 'lifecycle.jsonl'
        p.write_text(p.read_text()[:-1])
        self.expect(2)

    def test_common_missing_frame_row(self):
        # the same interior traversal missing on BOTH sides: rejected (original incomplete), never IDENTICAL
        rows = [r for r in FRAMES if r['t'] != 2]
        self.f.set_frames(rows)
        self.f.set_frames([dict(r, vb=0, opacity_bits=None, repeat=0) for r in rows], original=True)
        self.f.pin(frames=cmp.anchors(rows))
        self.expect(2, 'incomplete')

    def test_native_missing_interior_frame(self):
        self.f.set_frames([r for r in FRAMES if r['t'] != 2])
        self.expect(1, 'INCOMPLETE native coverage')

    def test_native_missing_tail_frame(self):
        self.f.set_frames(FRAMES[:-1])
        self.expect(1, 'INCOMPLETE native coverage')

    def test_native_frames_after_death(self):
        self.f.set_frames(FRAMES + [dict(FRAMES[-1], t=6, state_in=0, state_out=0)])
        self.expect(1, 'INCOMPLETE native coverage')

    def test_native_frame_keys(self):
        self.f.set_frames([dict(r, extra=1) for r in FRAMES])
        self.expect(2)

    def test_timing_difference_with_unexplained_rng_is_different(self):
        rows = [FRAMES[0], FRAMES[1], dict(FRAMES[1], t=2)] + [dict(r, t=r['t'] + 1) for r in FRAMES[2:]]
        self.f.set_frames(rows)
        for cp in CPS:
            if RNG_AT[cp] == 3:
                self.f.rewrite(cp, 'rng.jsonl', RNG + [{'i': 3, 'phase': 'restore-ui-glyph', 'state': 1, 'value': 1}])
        out = self.expect(1, 'RESULT: DIFFERENT')
        self.assertIn('timing diagnostic', out)

    def test_timing_different_with_corrupt_state_is_different(self):
        rows = [FRAMES[0], FRAMES[1], dict(FRAMES[1], t=2)] + [dict(r, t=r['t'] + 1) for r in FRAMES[2:]]
        self.f.set_frames(rows)
        s = state('second-title-return')
        s['free_records'] = 3
        (self.f.nat / 'second-title-return' / 'state.json').write_text(json.dumps(s))
        self.expect(1, 'RESULT: DIFFERENT')

    def test_same_anchors_frame_difference_is_different(self):
        rows = [dict(r) for r in FRAMES]
        rows[1]['update_state_out'] = 530
        self.f.set_frames(rows)
        self.expect(1, 'manager frames: DIFFERENT')

    def test_elf_mismatch(self):
        (self.f.nat / 'route.json').write_text(json.dumps(self.f.route_json(elf_sha256='0' * 64)))
        self.expect(2)

    def test_route_mismatch(self):
        (self.f.nat / 'route.json').write_text(json.dumps(self.f.route_json(route='restore')))
        self.expect(2)

    def test_route_extra_key(self):
        (self.f.nat / 'route.json').write_text(json.dumps(self.f.route_json(extra=1)))
        self.expect(2)

    def test_unpinned_original(self):
        p = self.f.orig / 'empty-ready' / 'rng.jsonl'
        p.write_text(p.read_text() + jl([{'i': 9, 'phase': 'session', 'state': 1, 'value': 1}]))
        self.expect(2, 'pinned')

    def test_corrupt_manifest(self):
        self.f.manifest.write_text('{"inputs": ')
        self.expect(2)

    def test_duplicate_manifest_key(self):
        t = self.f.manifest.read_text()
        self.f.manifest.write_text(t[:-1] + ', "outputs": {}}')
        self.expect(2)

    def test_manifest_digest_mismatch(self):
        m = json.loads(self.f.manifest.read_text())
        m['result_digest']['checkpoints']['first-layout']['rng_draws'] = 99
        self.f.manifest.write_text(json.dumps(m))
        self.expect(2)

    def test_manifest_frame_digest_mismatch(self):
        m = json.loads(self.f.manifest.read_text())
        m['result_digest']['manager_frames']['death_t'] = 9
        self.f.manifest.write_text(json.dumps(m))
        self.expect(2)

    def test_self_comparison(self):
        c = cmp.main([str(self.f.orig), str(self.f.orig), '--manifest', str(self.f.manifest)])
        self.assertEqual(c, 2)
        with redirect_stderr(io.StringIO()):
            self.assertEqual(cmp.main([str(self.f.orig), str(self.f.orig / 'empty-ready'), '--manifest', str(self.f.manifest)]), 2)

    def test_hard_linked_original(self):
        p = self.f.nat / 'empty-ready' / 'rng.jsonl'
        p.unlink()
        os.link(self.f.orig / 'empty-ready' / 'rng.jsonl', p)
        self.expect(2, 'self-comparison')

    def test_strict_rejects_coverage_gaps(self):
        for cp in CPS:
            k = bytearray((self.f.nat / cp / 'vram.known').read_bytes())
            k[0x30] = 0
            (self.f.nat / cp / 'vram.known').write_bytes(bytes(k))
            inv = bytearray(SMALL)
            inv[0x30] = 3
            (self.f.nat / cp / 'vram.invalidated').write_bytes(bytes(inv))
        self.expect(0)
        self.expect(1, 'strict', strict=True)

class TimingDiagnosticUnits(unittest.TestCase):
    """readiness() and pass_model(): the card-readiness anchors and the status -> pass-label model."""

    @staticmethod
    def frames(updates, draws):
        return [{'t': t, 'update_state_out': u, 'draws': d} for t, (u, d) in enumerate(zip(updates, draws))]

    @staticmethod
    def rng(labels):
        return [{'phase': x} for x in labels]

    def test_readiness(self):
        f = self.frames([510, 530, 530, 540], [1, 1, 1, 1])
        self.assertEqual(cmp.readiness(f), {'card_info_t': 1, 'card_ready_t': 3})

    def test_consistent_shift_has_the_same_model(self):
        # original: info at t2; native: info at t1 -> its 530 pass appears one traversal earlier
        P = {'c': ['g'], 510: ['g', 'i'], 530: ['g', 'g', 'i'], 540: ['i']}
        def run(updates):
            f = self.frames(updates, [0] * len(updates))
            labels = []
            for r in f:
                key = 'c' if r['t'] == 0 else f[max(r['t'] - 2, 0)]['update_state_out']
                r['draws'] = len(P[key]); labels += P[key]
            return f, self.rng(labels)
        o, n = run([510, 510, 530, 540, 540, 540]), run([510, 530, 540, 540, 540, 540])
        mo, mn = cmp.pass_model(*o), cmp.pass_model(*n)
        self.assertIsNotNone(mo)
        self.assertIsNotNone(mn)
        self.assertTrue(all(mo[k] == mn[k] for k in mo.keys() & mn.keys()))

    def test_relabelled_pass_breaks_the_model(self):
        f = self.frames([510, 510, 510, 510], [2, 2, 2, 2])
        r = self.rng(['g', 'g', 'g', 'i', 'g', 'i', 'i', 'g'])  # status 510 shows two different patterns
        self.assertIsNone(cmp.pass_model(f, r))

class OriginalDerivedControls(unittest.TestCase):
    """SYNTHETIC native-like fixture from the ORIGINAL extract; not native output."""

    @classmethod
    def setUpClass(cls):
        if not (EVIDENCE / 'reference.json').exists() or not cmp.MANIFEST.exists():
            raise unittest.SkipTest('Restore original evidence not present (set OKAGE_MAIN_CHECKOUT)')

    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.nat = Path(self.td.name) / 'native'
        for cp in CPS:
            o, n = EVIDENCE / cp, self.nat / cp
            n.mkdir(parents=True)
            shutil.copyfile(o / 'events.jsonl', n / 'lifecycle.jsonl')
            for x in ('state.json', 'effects.json', 'rng.jsonl'):
                shutil.copyfile(o / x, n / x)
            shutil.copyfile(o / 'vram-written-values.bin', n / 'vram.bytes')
            shutil.copyfile(o / 'vram-written-mask.bin', n / 'vram.known')
            shutil.copyfile(o / 'vram-invalidated-mask.bin', n / 'vram.invalidated')
        rows = [json.loads(x) for x in (EVIDENCE / 'manager-frames.jsonl').read_text().splitlines()]
        (self.nat / 'manager-frames.jsonl').write_text(jl([{k: r[k] for k in cmp.FRAME_KEYS} for r in rows]))
        ref = json.loads((EVIDENCE / 'reference.json').read_text())
        frames = {cp: ref['checkpoints'][cp]['t'] + 1000 for cp in CPS}
        (self.nat / 'route.json').write_text(json.dumps({'route': 'restore-cancel', 'elf_sha256': ELF, 'checkpoints': frames}))

    def tearDown(self):
        self.td.cleanup()

    def run_cmp(self):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            code = cmp.main([str(EVIDENCE), str(self.nat)])
        return code, out.getvalue()

    def test_faithful_copy_identical(self):
        code, out = self.run_cmp()
        self.assertEqual(code, 0, out)

    def test_atlas_bytes_corruption(self):
        for cp in CPS[1:]:
            p = self.nat / cp / 'lifecycle.jsonl'
            rows = [json.loads(x) for x in p.read_text().splitlines()]
            k = max(i for i, r in enumerate(rows[:len(rows)]) if r['kind'] == 'upload-bytes' and i < len(rows))
            first_post = [i for i, r in enumerate(rows) if r['kind'] == 'upload-bytes']
            # corrupt the bm_if image upload (the first upload after the Retire successor entry)
            pre = sum(1 for x in (EVIDENCE / 'retire-successor-entry' / 'events.jsonl').read_text().splitlines()
                      if json.loads(x)['kind'] == 'upload-bytes')
            idx = first_post[pre]
            rows[idx]['sha256'] = '0' * 64
            del k
            p.write_text(jl(rows))
        code, out = self.run_cmp()
        self.assertEqual(code, 1, out)

    def test_omitted_window_death(self):
        for cp in CPS[5:]:
            p = self.nat / cp / 'lifecycle.jsonl'
            rows = [json.loads(x) for x in p.read_text().splitlines()]
            p.write_text(jl([r for r in rows if not (r['kind'] == 'ui' and r['op'] == 'window-death')]))
        code, out = self.run_cmp()
        self.assertEqual(code, 1, out)

    def test_stale_known_at_cancel(self):
        inv = (EVIDENCE / 'cancel-death' / 'vram-invalidated-mask.bin').read_bytes()
        i = next(i for i, b in enumerate(inv) if b)
        k = bytearray((self.nat / 'cancel-death' / 'vram.known').read_bytes())
        k[i] |= inv[i]
        (self.nat / 'cancel-death' / 'vram.known').write_bytes(bytes(k))
        v = bytearray((self.nat / 'cancel-death' / 'vram.invalidated').read_bytes())
        v[i] = 0
        (self.nat / 'cancel-death' / 'vram.invalidated').write_bytes(bytes(v))
        code, out = self.run_cmp()
        self.assertEqual(code, 1, out)

    def test_self_comparison_refused(self):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            self.assertEqual(cmp.main([str(EVIDENCE), str(EVIDENCE)]), 2)

class ExtractorUnits(unittest.TestCase):
    def ev(self, rows):
        return [dict(r, i=i, vb=1, top=1) for i, r in enumerate(rows)]

    def test_return_checkpoint_boundary_follows_the_call(self):
        ev = self.ev([{'ev': 'enter', 'fn': '0x100f90', 'name': 'resource_tick'},
                      {'ev': 'exit', 'fn': '0x100f90', 'name': 'resource_tick', 'enter': 0},
                      {'ev': 'checkpoint', 'name': 'first-layout', 'pc': '0x115008'},
                      {'ev': 'enter', 'fn': '0x11e480', 'name': 'restore_input'},
                      {'ev': 'checkpoint', 'name': 'empty-ready', 'pc': '0x11e480'}])
        b = ext.checkpoint_boundaries(ev)
        self.assertEqual(b['first-layout'][1], 2)
        self.assertEqual(b['empty-ready'][1], 3)

    def test_checkpoint_not_after_trigger(self):
        ev = self.ev([{'ev': 'enter', 'fn': '0x11e480', 'name': 'restore_update'},
                      {'ev': 'checkpoint', 'name': 'empty-ready', 'pc': '0x11e480'}])
        with self.assertRaises(ext.Fail):
            ext.checkpoint_boundaries(ev)

    def test_owner_rows_and_ui_states(self):
        own = lambda k, m, s: {'node_kind': k, 'owner_node': '0x1', 'owner_kind': 'mode9_10', 'owner_mode': m, 'owner_selector': s}  # noqa: E731
        ev = self.ev([
            {'ev': 'top', 'pc': '0x100618'},
            {'ev': 'enter', 'name': 'restore_input', 'fn': '0x11e480'},
            {'ev': 'exit', 'name': 'restore_input', 'enter': 1, 'restore': {'state_1f89a8': 510}},
            {'ev': 'enter', 'name': 'restore_update', 'fn': '0x11c2c8'},
            {'ev': 'exit', 'name': 'restore_update', 'enter': 3, 'restore': {'state_1f89a8': 510}},
            {'ev': 'enter', 'name': 'resource_retire', 'fn': '0x101058', 'owner': own('resource', 10, 0)},
            {'ev': 'enter', 'name': 'wrapper_destroy', 'fn': '0x124628', 'wrapper': {'w24_mode': 10, 'w28_selector': 0}},
            {'ev': 'enter', 'name': 'host_free', 'fn': '0x1bdaf0', 'scheduler_node': {}, 'owner': own('mode9_10', 10, 0)},
            {'ev': 'enter', 'name': 'host_free', 'fn': '0x1bdaf0', 'scheduler_node': {}, 'owner': own('manager', 10, 0)},
        ])
        rows = ext.timeline(ev)[0]
        self.assertEqual(rows, [{'kind': 'ui', 'op': 'state', 'state': 510},
                                {'kind': 'mode', 'op': 'resource-retire', 'mode': 10, 'selector': 0},
                                {'kind': 'mode', 'op': 'destroy', 'mode': 10, 'selector': 0},
                                {'kind': 'mode', 'op': 'wrapper-free', 'node': 'mode10', 'mode': 10, 'selector': 0},
                                {'kind': 'mode', 'op': 'wrapper-free', 'node': 'manager', 'mode': 10, 'selector': 0}])

    def test_unmodelled_owner(self):
        ev = self.ev([{'ev': 'enter', 'name': 'host_free', 'fn': '0x1bdaf0', 'scheduler_node': {},
                       'owner': {'node_kind': 'other', 'owner_node': '0x1', 'owner_kind': 'other', 'owner_mode': -1,
                                 'owner_selector': -1}}])
        with self.assertRaises(ext.Fail):
            ext.timeline(ev)

    def test_frame_coverage(self):
        self.assertEqual(cmp.frame_coverage(FRAMES), [])
        self.assertTrue(cmp.frame_coverage([r for r in FRAMES if r['t'] != 3]))

if __name__ == '__main__':
    unittest.main()
