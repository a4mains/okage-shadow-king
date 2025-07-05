"""Controls for tools/retire_title_compare.py and tools/retire_title_original_extract.py.

1. SyntheticControls: a small, hand-written original/native pair (SYNTHETIC; the comparator's VRAM size is reduced to
   4 KiB for these tests only) exercising every comparator rule and negative control.
2. OriginalDerivedControls: a SYNTHETIC native-like fixture built from the pinned Retire ORIGINAL extract (each original
   checkpoint file copied into the native layout; vram.bytes = written values, vram.known = written mask,
   vram.invalidated = the original invalidated mask). It is not native output: it proves only that the comparator
   accepts a faithful report and rejects the listed corruptions at full size. Runs when the ignored evidence is
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
import retire_title_compare as cmp  # noqa: E402
import retire_title_original_extract as ext  # noqa: E402

MAIN = Path(os.environ.get('OKAGE_MAIN_CHECKOUT', ROOT))
EVIDENCE = MAIN / 'local/research/retire-title-original-v1/analysis'
ELF = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
SMALL = 4096
CPS = cmp.CHECKPOINTS

def jl(rows):
    return ''.join(json.dumps(r) + '\n' for r in rows)

def cov(cp):
    return {'lifecycle': 'retire-title', 'checkpoint': cp, 'memory': cmp.MEMORY}

# Five-checkpoint synthetic stream (cumulative).
STREAM = [
    [{'kind': 'phase', 'name': 'prefix-end-0x100618'},
     {'kind': 'mode', 'op': 'request', 'mode': 8, 'selector': 1, 'arg': 0},
     {'kind': 'alloc', 'alloc': 0, 'class': 1, 'psm': 19, 'w': 16, 'h': 16, 'tbp': 100},
     {'kind': 'ref', 'alloc': 0, 'delta': 1, 'refs': 1},
     {'kind': 'handle', 'handle': 5, 'record': 7, 'alias': False},
     {'kind': 'upload', 'alloc': 0}],
    [{'kind': 'effect', 'op': 'create', 'shape': 106, 'slot': 0},
     {'kind': 'effect', 'op': 'bind', 'slot': 0, 'element': 0, 'handle': 5}],
    [{'kind': 'phase', 'name': 'title-submode-30'}],
    [{'kind': 'effect', 'op': 'remove-request', 'slot': 0},
     {'kind': 'mode', 'op': 'request', 'mode': 10, 'selector': 0, 'arg': 0},
     {'kind': 'mode', 'op': 'resource-retire', 'mode': 8, 'selector': 1},
     {'kind': 'effect', 'op': 'remove-request', 'slot': -2},
     {'kind': 'retire', 'handle': 5},
     {'kind': 'ref', 'alloc': 0, 'delta': -1, 'refs': 0},
     {'kind': 'free', 'alloc': 0}],
    [{'kind': 'mode', 'op': 'destroy', 'mode': 8, 'selector': 1},
     {'kind': 'mode', 'op': 'wrapper-free', 'node': 'resource', 'mode': 8, 'selector': 1},
     {'kind': 'mode', 'op': 'wrapper-free', 'node': 'mode8', 'mode': 8, 'selector': 1}],
]
# The stream grows between checkpoints: first-update sees STREAM[0], stable-menu STREAM[0]+[1], ...
PREFIX = {cp: sum(STREAM[:i + 1], []) for i, cp in enumerate(CPS)}
EFFECT = {'first-update': None,
          'stable-menu': {'slot': 0, 'shape': 106, 'state': 'active', 'element_handles': [5]},
          'removal-request': {'slot': 0, 'shape': 106, 'state': 'active', 'element_handles': [5]},
          'resource-retirement': {'slot': 0, 'shape': 106, 'state': 'reclaim-pending', 'element_handles': [5]},
          'successor-entry': {'slot': 0, 'shape': 106, 'state': 'reclaim-pending', 'element_handles': [5]}}
RNG = [{'i': 0, 'phase': 'noise', 'state': 12345, 'value': 12345}, {'i': 1, 'phase': 'effect-grid', 'state': 7, 'value': 7}]

def state(cp):
    live = cp in ('first-update', 'stable-menu', 'removal-request')
    return {
        'coverage': cov(cp),
        'allocations': [{'id': 0, 'serial': 1, 'class': 1, 'psm': 19, 'width': 16, 'height': 16, 'tbp': 100, 'tbw': 1,
                         'references': 1, 'descriptor': 999}] if live else [],
        'handles': [{'handle': 5, 'record': 7, 'image': [0, 1], 'palettes': [None]}] if live else [],
        'records': [{'record': 7, 'name': 'hanten.xim', 'hash_slot': 3, 'handles': 1}] if live else [],
        'hash': [[3, 8]] if live else [], 'reserved': [], 'free_records': 511, 'free_extras': 128,
        'available': 4095 if live else 4096, 'cursors': [0] * 64, 'free_queue_ids': [1, 2, 3],
        'occupancy_hex': '00' * 8192, 'image_list': [5] if live else [], 'materials': [[0, 5]] if live else [],
    }

class Fixture:
    def __init__(self, tmp, route='restore'):
        self.tmp = Path(tmp)
        self.route = route
        self.orig = self.tmp / 'analysis'
        self.nat = self.tmp / 'native'
        self.manifest = self.tmp / 'manifest.json'
        mask, vals, inval = bytearray(SMALL), bytearray(SMALL), bytearray(SMALL)
        mask[0x10], vals[0x10] = 3, 0x5a
        mask[0x11], vals[0x11] = 1, 0x07
        inval[0x20] = 3                      # last written by a draw
        inval[0x21] = 2
        mask[0x30], vals[0x30] = 3, 0x99
        self.mask, self.vals, self.inval = mask, vals, inval
        for cp in CPS:
            d = self.orig / route / cp
            d.mkdir(parents=True)
            (d / 'events.jsonl').write_text(jl(PREFIX[cp]))
            (d / 'state.json').write_text(json.dumps(state(cp)))
            (d / 'effects.json').write_text(json.dumps({'effect106': EFFECT[cp]}))
            (d / 'rng.jsonl').write_text(jl(RNG))
            (d / 'vram-written-mask.bin').write_bytes(bytes(mask))
            (d / 'vram-written-values.bin').write_bytes(bytes(vals))
            (d / 'vram-invalidated-mask.bin').write_bytes(bytes(inval))
            n = self.nat / cp
            n.mkdir(parents=True)
            (n / 'lifecycle.jsonl').write_text(jl(PREFIX[cp]))
            (n / 'state.json').write_text(json.dumps(state(cp)))
            (n / 'effects.json').write_text(json.dumps({'effect106': EFFECT[cp], 'grid': [1, 2]}))
            (n / 'rng.jsonl').write_text(jl(RNG))
            data = bytearray(vals)
            data[0x11] = 0xf7                # unknown high nibble: value irrelevant
            (n / 'vram.bytes').write_bytes(bytes(data))
            (n / 'vram.known').write_bytes(bytes(mask))
        (self.nat / 'route.json').write_text(json.dumps(self.route_json()))
        self.pin()

    def route_json(self, **kw):
        r = {'route': self.route, 'elf_sha256': ELF, 'checkpoints': {cp: 100 + 10 * i for i, cp in enumerate(CPS)}}
        r.update(kw)
        return r

    def pin(self, elf=ELF):
        outs, dig = {}, {}
        for cp in CPS:
            for n in cmp.ORIGINAL_FILES:
                p = self.orig / self.route / cp / n
                if p.exists():
                    b = p.read_bytes()
                    outs[f'analysis/{self.route}/{cp}/{n}'] = {'sha256': hashlib.sha256(b).hexdigest(), 'bytes': len(b)}
            dig[cp] = {'resource_events': sum(1 for e in PREFIX[cp] if e['kind'] != 'phase'), 'rng_draws': len(RNG),
                       'vram_known_nibbles': 5, 'vram_unknown_nibbles': 3}
        self.manifest.write_text(json.dumps({'inputs': {'elf': {'sha256': elf}}, 'outputs': outs,
                                             'result_digest': {'routes': {self.route: dig}}}))

    def run(self, nat=None, orig=None):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = cmp.main([str(orig or self.orig), str(nat or self.nat), '--manifest', str(self.manifest)])
        return code, out.getvalue() + err.getvalue()

    def edit_lifecycle(self, cp, fn):
        p = self.nat / cp / 'lifecycle.jsonl'
        p.write_text(jl(fn([json.loads(l) for l in p.read_text().splitlines()])))

    def edit_json(self, cp, name, fn):
        p = self.nat / cp / name
        d = json.loads(p.read_text())
        d = fn(d) or d
        p.write_text(json.dumps(d))

    def edit_bytes(self, cp, name, offset, value):
        p = self.nat / cp / name
        b = bytearray(p.read_bytes()) if p.exists() else bytearray(SMALL)
        b[offset] = value
        p.write_bytes(bytes(b))

class SyntheticControls(unittest.TestCase):
    def setUp(self):
        self._vram = cmp.VRAM
        cmp.VRAM = SMALL
        self._tmp = tempfile.TemporaryDirectory()
        self.f = Fixture(self._tmp.name)

    def tearDown(self):
        cmp.VRAM = self._vram
        self._tmp.cleanup()

    def assertCode(self, code, text=None):
        got, out = self.f.run()
        self.assertEqual(got, code, out)
        if text:
            self.assertIn(text, out)
        return out

    def test_identical(self):
        self.assertCode(0, 'RESULT: IDENTICAL')

    def test_phase_rows_are_informational(self):
        self.f.edit_lifecycle('stable-menu', lambda r: [e for e in r if e['kind'] != 'phase'] + [{'kind': 'phase', 'name': 'x'}])
        self.assertCode(0)

    # -- events
    def test_event_omission_fails(self):
        self.f.edit_lifecycle('resource-retirement', lambda r: [e for e in r if e.get('op') != 'remove-request'])
        self.assertCode(1, 'events: DIFFERENT')

    def test_event_reordering_fails(self):
        def f(r):
            i = next(k for k, e in enumerate(r) if e.get('op') == 'destroy')
            r[i], r[i + 1] = r[i + 1], r[i]
            return r
        self.f.edit_lifecycle('successor-entry', f)
        self.assertCode(1, 'events: DIFFERENT')

    def test_wrapper_free_before_destroy_fails(self):
        def f(r):
            w = [e for e in r if e.get('op') == 'wrapper-free']
            rest = [e for e in r if e.get('op') != 'wrapper-free']
            i = next(k for k, e in enumerate(rest) if e.get('op') == 'destroy')
            return rest[:i] + w + rest[i:]
        self.f.edit_lifecycle('successor-entry', f)
        self.assertCode(1)

    def test_removal_request_is_not_reclamation(self):
        def f(r):
            for e in r:
                if e.get('op') == 'remove-request' and e['slot'] == 0:
                    e['op'] = 'reclaim'
            return r
        self.f.edit_lifecycle('resource-retirement', f)
        self.assertCode(1)

    def test_changed_binding_fails(self):
        def f(r):
            for e in r:
                if e.get('op') == 'bind':
                    e['handle'] = 4
            return r
        self.f.edit_lifecycle('stable-menu', f)
        self.assertCode(1, 'events: DIFFERENT')

    def test_changed_refcount_fails(self):
        def f(r):
            next(e for e in r if e['kind'] == 'ref')['refs'] = 2
            return r
        self.f.edit_lifecycle('first-update', f)
        self.assertCode(1)

    def test_effect_and_mode_schema(self):
        self.f.edit_lifecycle('first-update', lambda r: r + [{'kind': 'effect', 'op': 'update', 'slot': 0}])
        self.assertCode(2, 'unknown effect op')

    def test_effect_extra_key_is_malformed(self):
        def f(r):
            next(e for e in r if e.get('op') == 'create')['flags'] = 1
            return r
        self.f.edit_lifecycle('stable-menu', f)
        self.assertCode(2, 'keys')

    def test_wrapper_free_node_vocabulary(self):
        def f(r):
            next(e for e in r if e.get('op') == 'wrapper-free')['node'] = 'other'
            return r
        self.f.edit_lifecycle('successor-entry', f)
        self.assertCode(2)

    def test_bool_is_not_an_integer(self):
        def f(r):
            next(e for e in r if e.get('op') == 'create')['slot'] = False
            return r
        self.f.edit_lifecycle('stable-menu', f)
        self.assertCode(2)

    def test_truncated_lifecycle_file_is_malformed(self):
        p = self.f.nat / 'stable-menu' / 'lifecycle.jsonl'
        p.write_text(p.read_text()[:-3])
        self.assertCode(2, 'truncated')

    def test_truncated_checkpoint_stream_fails(self):
        self.f.edit_lifecycle('resource-retirement', lambda r: r[:-2])
        self.assertCode(1, 'events: DIFFERENT')

    def test_duplicated_checkpoint_fails(self):
        shutil.rmtree(self.f.nat / 'resource-retirement')
        shutil.copytree(self.f.nat / 'removal-request', self.f.nat / 'resource-retirement')
        self.f.edit_json('resource-retirement', 'state.json', lambda s: s.update(coverage=cov('resource-retirement')))
        self.assertCode(1, 'events: DIFFERENT')

    def test_duplicated_frame_is_malformed(self):
        r = self.f.route_json()
        r['checkpoints']['stable-menu'] = r['checkpoints']['first-update']
        (self.f.nat / 'route.json').write_text(json.dumps(r))
        self.assertCode(2, 'strictly increasing')

    def test_missing_checkpoint_directory_is_malformed(self):
        shutil.rmtree(self.f.nat / 'successor-entry')
        self.assertCode(2, 'missing')

    def test_stream_must_extend_previous_checkpoint(self):
        # native stable-menu inserts an event that later checkpoints do not have (and the original does not)
        self.f.edit_lifecycle('stable-menu', lambda r: r + [{'kind': 'upload', 'alloc': 0}])
        out = self.assertCode(1)
        self.assertIn('stream_inconsistent', out)

    def test_empty_lifecycle_is_malformed(self):
        (self.f.nat / 'first-update' / 'lifecycle.jsonl').write_text('')
        self.assertCode(2, 'no lifecycle events')

    # -- state / effect / rng
    def test_state_difference_fails(self):
        self.f.edit_json('successor-entry', 'state.json', lambda s: s.update(image_list=[5]))
        self.assertCode(1, 'state.image_list: DIFFERENT')

    def test_survivor_refcount_difference_fails(self):
        self.f.edit_json('stable-menu', 'state.json', lambda s: s['allocations'][0].update(references=2))
        self.assertCode(1, 'state.allocations: DIFFERENT')

    def test_wrong_coverage_is_malformed(self):
        self.f.edit_json('stable-menu', 'state.json', lambda s: s.update(coverage=cov('first-update')))
        self.assertCode(2, 'coverage')

    def test_unknown_state_key_is_malformed(self):
        self.f.edit_json('stable-menu', 'state.json', lambda s: s.update(effect106=1))
        self.assertCode(2, 'unknown keys')

    def test_duplicate_json_key_is_malformed(self):
        (self.f.nat / 'stable-menu' / 'effects.json').write_text('{"effect106": null, "effect106": null}')
        self.assertCode(2, 'duplicate key')

    def test_effect_binding_difference_fails(self):
        self.f.edit_json('resource-retirement', 'effects.json', lambda d: d['effect106'].update(element_handles=[4]))
        self.assertCode(1, 'effect106: DIFFERENT')

    def test_effect_state_difference_fails(self):
        self.f.edit_json('successor-entry', 'effects.json', lambda d: d['effect106'].update(state='reclaimed'))
        self.assertCode(1)

    def test_effect_state_vocabulary(self):
        self.f.edit_json('successor-entry', 'effects.json', lambda d: d['effect106'].update(state='gone'))
        self.assertCode(2)

    def test_rng_difference_fails(self):
        p = self.f.nat / 'removal-request' / 'rng.jsonl'
        p.write_text(jl(RNG[:1] + [{'i': 1, 'phase': 'effect-grid', 'state': 8, 'value': 8}]))
        self.assertCode(1, 'rng: DIFFERENT at draw 1')

    def test_rng_extra_draw_fails(self):
        for cp in CPS[2:]:
            (self.f.nat / cp / 'rng.jsonl').write_text(jl(RNG + [{'i': 2, 'phase': 'session', 'state': 1, 'value': 1}]))
        self.assertCode(1, 'rng: DIFFERENT at draw 2')

    def test_rng_index_gap_is_malformed(self):
        (self.f.nat / 'first-update' / 'rng.jsonl').write_text(jl([dict(RNG[0], i=1)]))
        self.assertCode(2)

    # -- VRAM
    def test_missing_knowledge_fails(self):
        self.f.edit_bytes('stable-menu', 'vram.known', 0x10, 1)
        self.assertCode(1, 'missing_knowledge=1')

    def test_value_mismatch_fails(self):
        self.f.edit_bytes('stable-menu', 'vram.bytes', 0x10, 0x5b)
        self.assertCode(1, 'value_mismatch=1')

    def test_stale_known_fails(self):
        self.f.edit_bytes('successor-entry', 'vram.known', 0x21, 2)
        self.assertCode(1, 'stale_known=1')

    def test_invented_knowledge_fails(self):
        self.f.edit_bytes('first-update', 'vram.known', 0x11, 3)
        self.assertCode(1, 'invented_knowledge=1')

    def test_invalidated_upload_nibble_is_coverage_not_failure(self):
        self.f.edit_bytes('removal-request', 'vram.known', 0x30, 0)
        self.f.edit_bytes('removal-request', 'vram.invalidated', 0x30, 3)
        self.assertCode(0, 'coverage_gap_invalidated=2')

    def test_unknown_without_invalidation_fails(self):
        self.f.edit_bytes('removal-request', 'vram.known', 0x30, 0)
        self.assertCode(1, 'missing_knowledge=2')

    def test_known_and_invalidated_overlap_is_malformed(self):
        self.f.edit_bytes('removal-request', 'vram.invalidated', 0x10, 1)
        self.assertCode(2, 'overlap')

    def test_extra_mask_bits_are_malformed(self):
        self.f.edit_bytes('stable-menu', 'vram.known', 0x40, 4)
        self.assertCode(2, 'bit0/bit1')

    def test_short_vram_is_malformed(self):
        (self.f.nat / 'stable-menu' / 'vram.bytes').write_bytes(b'\0' * 10)
        self.assertCode(2)

    # -- identities, pins, self-comparison
    def test_elf_identity_mismatch_is_malformed(self):
        (self.f.nat / 'route.json').write_text(json.dumps(self.f.route_json(elf_sha256='0' * 64)))
        self.assertCode(2, 'ELF identity')

    def test_unknown_route_is_malformed(self):
        (self.f.nat / 'route.json').write_text(json.dumps(self.f.route_json(route='attract')))
        self.assertCode(2, 'unknown route')

    def test_route_without_pinned_original_is_malformed(self):
        (self.f.nat / 'route.json').write_text(json.dumps(self.f.route_json(route='newgame')))
        self.assertCode(2, 'does not pin')

    def test_route_json_extra_key_is_malformed(self):
        (self.f.nat / 'route.json').write_text(json.dumps(self.f.route_json(seed=1)))
        self.assertCode(2)

    def test_unpinned_original_is_malformed(self):
        p = self.f.orig / 'restore' / 'stable-menu' / 'events.jsonl'
        p.write_text(p.read_text() + p.read_text().splitlines()[-1] + '\n')
        self.assertCode(2, 'pinned original')

    def test_digest_mismatch_is_malformed(self):
        m = json.loads(self.f.manifest.read_text())
        m['result_digest']['routes']['restore']['stable-menu']['resource_events'] += 1
        self.f.manifest.write_text(json.dumps(m))
        self.assertCode(2, 'digest')

    def test_self_comparison_is_refused(self):
        got, out = self.f.run(nat=self.f.orig)
        self.assertEqual(got, 2, out)
        got, out = self.f.run(nat=self.f.orig / 'restore')
        self.assertEqual(got, 2, out)

    def test_hard_linked_original_is_refused(self):
        p = self.f.nat / 'stable-menu' / 'state.json'
        p.unlink()
        os.link(self.f.orig / 'restore' / 'stable-menu' / 'state.json', p)
        self.assertCode(2, 'self-comparison')

    def test_malformed_route_json(self):
        (self.f.nat / 'route.json').write_text('{"route": "restore",')
        self.assertCode(2)

# ---------------------------------------------------------------------------------------------------
def build_native_from_original(orig_route_dir, nat, route, frames=None):
    """SYNTHETIC native-like report built from the ORIGINAL extract (not native output)."""
    nat.mkdir()
    for i, cp in enumerate(CPS):
        o, n = orig_route_dir / cp, nat / cp
        n.mkdir()
        shutil.copyfile(o / 'events.jsonl', n / 'lifecycle.jsonl')
        for name in ('state.json', 'effects.json', 'rng.jsonl'):
            shutil.copyfile(o / name, n / name)
        shutil.copyfile(o / 'vram-written-values.bin', n / 'vram.bytes')
        shutil.copyfile(o / 'vram-written-mask.bin', n / 'vram.known')
        shutil.copyfile(o / 'vram-invalidated-mask.bin', n / 'vram.invalidated')
    (nat / 'route.json').write_text(json.dumps({'route': route, 'elf_sha256': ELF,
                                                'checkpoints': frames or {cp: 1000 + i for i, cp in enumerate(CPS)}}))

@unittest.skipUnless((EVIDENCE / 'restore' / 'successor-entry' / 'state.json').is_file()
                     and (ROOT / 'docs/retire-title-original-manifest.json').is_file(),
                     'Retire original evidence absent (set OKAGE_MAIN_CHECKOUT)')
class OriginalDerivedControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.base = {}
        for route in cmp.ROUTES:
            cls.base[route] = Path(cls._tmp.name) / ('base-' + route)
            build_native_from_original(EVIDENCE / route, cls.base[route], route)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def fresh(self, route='restore'):
        d = Path(tempfile.mkdtemp(dir=self._tmp.name))
        nat = d / 'native'
        shutil.copytree(self.base[route], nat, copy_function=os.link)   # hard links among native files only
        return nat

    def run_cmp(self, nat):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = cmp.main([str(EVIDENCE), str(nat)])
        return code, out.getvalue() + err.getvalue()

    def replace_text(self, path, text):
        path.unlink()
        path.write_text(text)

    def edit_lifecycle(self, nat, cp, fn):
        p = nat / cp / 'lifecycle.jsonl'
        rows = [json.loads(l) for l in p.read_text().splitlines()]
        self.replace_text(p, jl(fn(rows)))

    def test_faithful_synthetic_report_is_identical_both_routes(self):
        for route in cmp.ROUTES:
            code, out = self.run_cmp(self.fresh(route))
            self.assertEqual(code, 0, out)
            self.assertIn('RESULT: IDENTICAL', out)

    def write_updates(self, nat, route, fn=lambda rows: rows):
        rows = [json.loads(l) for l in (EVIDENCE / route / 'title-frames.jsonl').read_text().splitlines()]
        rows = [{k: r[k] for k in cmp.TITLE_UPDATE_KEYS} for r in rows]
        p = nat / 'title-updates.jsonl'
        if p.exists():
            p.unlink()
        p.write_text(jl(fn(rows)))

    def run_strict(self, nat):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = cmp.main([str(EVIDENCE), str(nat), '--strict'])
        return code, out.getvalue() + err.getvalue()

    def test_strict_title_updates_identical_both_routes(self):
        for route in cmp.ROUTES:
            nat = self.fresh(route)
            self.write_updates(nat, route)
            code, out = self.run_strict(nat)
            self.assertEqual(code, 0, out)
            self.assertIn('title updates: identical', out)

    def test_strict_requires_title_updates(self):
        code, out = self.run_strict(self.fresh())
        self.assertEqual(code, 2, out)

    def test_title_updates_not_compared_without_strict(self):
        nat = self.fresh()
        self.write_updates(nat, 'restore', lambda r: r[:5])
        code, out = self.run_cmp(nat)
        self.assertEqual(code, 0, out)
        self.assertIn('title updates: not compared', out)

    def test_strict_changed_fade_bits_fail(self):
        nat = self.fresh()

        def f(rows):
            rows[50]['fade_b8'] = '0x3f000001'
            return rows
        self.write_updates(nat, 'restore', f)
        code, out = self.run_strict(nat)
        self.assertEqual(code, 1, out)
        self.assertIn('DIFFERENT at update 50', out)

    def test_strict_late_removal_fails(self):
        nat = self.fresh()

        def f(rows):
            extra = dict(rows[-1])
            extra['title_n'] = len(rows)
            return rows + [extra]
        self.write_updates(nat, 'restore', f)
        code, out = self.run_strict(nat)
        self.assertEqual(code, 1, out)

    def test_strict_truncated_updates_fail(self):
        nat = self.fresh()
        self.write_updates(nat, 'restore', lambda r: r[:-1])
        code, out = self.run_strict(nat)
        self.assertEqual(code, 1, out)

    def test_strict_renumbered_or_extra_key_is_malformed(self):
        nat = self.fresh()

        def f(rows):
            rows[3]['title_n'] = 7
            return rows
        self.write_updates(nat, 'restore', f)
        self.assertEqual(self.run_strict(nat)[0], 2)
        nat = self.fresh()

        def g(rows):
            rows[3]['extra'] = 1
            return rows
        self.write_updates(nat, 'restore', g)
        self.assertEqual(self.run_strict(nat)[0], 2)

    def test_strict_rejects_coverage_gaps(self):
        nat = self.fresh()
        self.write_updates(nat, 'restore')
        cp = nat / 'first-update'
        known = bytearray((cp / 'vram.known').read_bytes())
        inval = bytearray((cp / 'vram.invalidated').read_bytes())
        i = next(k for k, v in enumerate(known) if v & 1 and not inval[k])
        known[i] &= ~1
        inval[i] |= 1
        for name, data in (('vram.known', known), ('vram.invalidated', inval)):
            (cp / name).unlink()
            (cp / name).write_bytes(bytes(data))
        code, out = self.run_cmp(nat)
        self.assertEqual(code, 0, out)
        code, out = self.run_strict(nat)
        self.assertEqual(code, 1, out)

    def test_route_swap_fails(self):
        nat = self.fresh('newgame')
        r = json.loads((nat / 'route.json').read_text())
        r['route'] = 'restore'
        self.replace_text(nat / 'route.json', json.dumps(r))
        code, out = self.run_cmp(nat)
        self.assertEqual(code, 1, out)

    def test_omitted_wrapper_free_fails(self):
        nat = self.fresh()
        self.edit_lifecycle(nat, 'successor-entry', lambda r: [e for e in r if not (e.get('op') == 'wrapper-free'
                                                                                     and e['selector'] == 1)])
        code, out = self.run_cmp(nat)
        self.assertEqual(code, 1, out)

    def test_reordered_retirement_fails(self):
        nat = self.fresh()

        def f(r):
            idx = [k for k, e in enumerate(r) if e['kind'] == 'retire']
            a, b = idx[-1], idx[-2]
            r[a], r[b] = r[b], r[a]
            return r
        self.edit_lifecycle(nat, 'successor-entry', f)
        code, out = self.run_cmp(nat)
        self.assertEqual(code, 1, out)

    def test_changed_effect_binding_fails(self):
        nat = self.fresh()

        def f(r):
            next(e for e in r if e.get('op') == 'bind')['handle'] = 42
            return r
        self.edit_lifecycle(nat, 'stable-menu', f)
        code, out = self.run_cmp(nat)
        self.assertEqual(code, 1, out)

    def test_stale_known_nibble_fails(self):
        nat = self.fresh()
        inval = (EVIDENCE / 'restore' / 'successor-entry' / 'vram-invalidated-mask.bin').read_bytes()
        i = next(k for k, b in enumerate(inval) if b & 1)
        known = bytearray((nat / 'successor-entry' / 'vram.known').read_bytes())
        known[i] |= 1
        inv = bytearray((nat / 'successor-entry' / 'vram.invalidated').read_bytes())
        inv[i] &= ~1
        (nat / 'successor-entry' / 'vram.known').unlink()
        (nat / 'successor-entry' / 'vram.known').write_bytes(bytes(known))
        (nat / 'successor-entry' / 'vram.invalidated').unlink()
        (nat / 'successor-entry' / 'vram.invalidated').write_bytes(bytes(inv))
        code, out = self.run_cmp(nat)
        self.assertEqual(code, 1, out)
        self.assertIn('stale_known=1', out)

    def test_truncated_checkpoint_fails(self):
        nat = self.fresh()
        self.edit_lifecycle(nat, 'resource-retirement', lambda r: r[:-1])
        code, out = self.run_cmp(nat)
        self.assertEqual(code, 1, out)

    def test_survivor_refcount_change_fails(self):
        nat = self.fresh()
        s = json.loads((nat / 'successor-entry' / 'state.json').read_text())
        s['allocations'][0]['references'] += 1
        self.replace_text(nat / 'successor-entry' / 'state.json', json.dumps(s))
        code, out = self.run_cmp(nat)
        self.assertEqual(code, 1, out)

    def test_self_comparison_is_refused(self):
        code, out = self.run_cmp(EVIDENCE / 'restore')
        self.assertEqual(code, 2, out)

# ---------------------------------------------------------------------------------------------------
class ExtractorUnits(unittest.TestCase):
    def ev(self, i, ev, name=None, **kw):
        d = {'i': i, 'ev': ev, 'vb': 1, 'top': 0}
        if name:
            d['name'] = name
            d.setdefault('fn', '0x0')
        d.update(kw)
        return d

    def test_effect_and_mode_events(self):
        evs = [
            self.ev(0, 'enter', 'mode_request', ra='0x1', mode=10, selector=0, arg3=0),
            self.ev(1, 'enter', 'effect_alloc', shape=106),
            self.ev(2, 'exit', 'effect_alloc', enter=1, v0='0x0', slot=0),
            self.ev(3, 'enter', 'effect_bind', slot=0, element=0, handle=43),
            self.ev(4, 'exit', 'effect_bind', enter=3, effect={'element_handles': [43]}),
            self.ev(5, 'enter', 'effect_remove_request', slot=0),
            self.ev(6, 'enter', 'resource_retire', mode8_selector=1),
            self.ev(7, 'enter', 'mode8_destroy', selector=1),
            self.ev(8, 'enter', 'host_free', scheduler_node={'kind': 'resource', 'mode8_selector': 1}),
            self.ev(9, 'enter', 'host_free', ptr='0x1'),
            self.ev(10, 'enter', 'effect_free', job_slot=-1),
            self.ev(11, 'enter', 'effect_free', job_slot=0),
        ]
        out, detail, counts = ext.timeline(evs)
        self.assertEqual(out, [
            {'kind': 'mode', 'op': 'request', 'mode': 10, 'selector': 0, 'arg': 0},
            {'kind': 'effect', 'op': 'create', 'shape': 106, 'slot': 0},
            {'kind': 'effect', 'op': 'bind', 'slot': 0, 'element': 0, 'handle': 43},
            {'kind': 'effect', 'op': 'remove-request', 'slot': 0},
            {'kind': 'mode', 'op': 'resource-retire', 'mode': 8, 'selector': 1},
            {'kind': 'mode', 'op': 'destroy', 'mode': 8, 'selector': 1},
            {'kind': 'mode', 'op': 'wrapper-free', 'node': 'resource', 'mode': 8, 'selector': 1},
            {'kind': 'effect', 'op': 'reclaim', 'slot': 0},
        ])
        for e in out:
            cmp_spec = cmp.OP_KEYS[e['kind']][e['op']]
            self.assertEqual(set(e), set(cmp_spec) | {'kind', 'op'})

    def test_failed_bind_is_rejected(self):
        evs = [self.ev(0, 'enter', 'effect_bind', slot=0, element=0, handle=43),
               self.ev(1, 'exit', 'effect_bind', enter=0, effect={'element_handles': [4]})]
        with self.assertRaises(ext.Fail):
            ext.timeline(evs)

    def test_unmodelled_scheduler_node_is_rejected(self):
        with self.assertRaises(ext.Fail):
            ext.timeline([self.ev(0, 'enter', 'host_free', scheduler_node={'kind': 'other', 'mode8_selector': -1})])

    def test_checkpoint_boundary_is_the_triggering_entry(self):
        evs = [self.ev(0, 'enter', 'effect_remove_request', fn='0x103640', slot=0),
               self.ev(1, 'checkpoint', name='removal-request', pc='0x103640')]
        evs[1]['name'] = 'removal-request'
        b = ext.checkpoint_boundaries(evs)
        self.assertEqual(b['removal-request'][1], 0)
        out, detail, _ = ext.timeline([evs[0]])
        self.assertEqual(ext.prefix(detail, out, 0), [])

    def test_checkpoint_not_after_its_trigger_is_rejected(self):
        evs = [self.ev(0, 'enter', 'title_update', fn='0x1296f8', title_n=0, submode=0),
               self.ev(1, 'checkpoint', name='removal-request', pc='0x103640')]
        evs[1]['name'] = 'removal-request'
        with self.assertRaises(ext.Fail):
            ext.checkpoint_boundaries(evs)

    def test_effect_state(self):
        self.assertIsNone(ext.effect_state(None))
        base = {'slot': 0, 'job': '0x10', 'type': 106, 'element_handles': [43]}
        self.assertEqual(ext.effect_state(dict(base, flags='0x8420'))['state'], 'active')
        self.assertEqual(ext.effect_state(dict(base, flags='0x8520'))['state'], 'removal-requested')
        self.assertEqual(ext.effect_state(dict(base, flags='0x8720'))['state'], 'reclaim-pending')
        self.assertEqual(ext.effect_state({'slot': 0, 'job': -1})['state'], 'reclaimed')

if __name__ == '__main__':
    unittest.main()
