import copy
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[1]
spec = importlib.util.spec_from_file_location('retire_title_contract', ROOT / 'tools/retire_title_contract.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
DOC = json.loads((ROOT / 'docs/retire-title-contract.json').read_text())
TITLE = json.loads((ROOT / 'docs/title-contract.json').read_text())

def events(route, kind, doc=DOC):
    return [e for e in doc['routes'][route]['events'] if e['kind'] == kind]

def renumber(doc, route):
    for i, e in enumerate(doc['routes'][route]['events']):
        e['seq'] = i
    doc['routes'][route]['counts']['events'] = len(doc['routes'][route]['events'])
    return doc

def local_root():
    cands = [os.environ.get('OKAGE_LOCAL'), str(ROOT / 'local')]
    try:
        common = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--path-format=absolute', '--git-common-dir'],
                                capture_output=True, text=True, check=True).stdout.strip()
        cands.append(str(Path(common).parent / 'local'))
    except (OSError, subprocess.CalledProcessError):
        pass
    for c in cands:
        if c and os.path.isfile(os.path.join(c, 'extracted/SCUS_971.29')):
            return c
    return None

class Contract(unittest.TestCase):
    def test_committed_contract_is_valid(self):
        self.assertEqual(mod.check(DOC), [])

    def test_baseline_is_pinned_title(self):
        self.assertEqual(DOC['baseline']['title_sha256'], mod.TITLE_SHA256)
        self.assertEqual(DOC['baseline']['image_list'], [36, 37, 38, 39, 40, 41, 42, 43])
        self.assertEqual((DOC['baseline']['live_allocations'], DOC['baseline']['live_handles']), (162, 111))

    def test_counts_from_source_reference(self):
        for r in mod.ROUTES:
            c = DOC['routes'][r]['counts']
            self.assertEqual((c['handles_retired'], c['records_destroyed'], c['frees']), (75, 75, 83))
            self.assertEqual((c['binding_retires'], c['image_list_retires']), (67, 8))
            self.assertEqual((c['hash_tombstones'], c['null_descriptors']), (62, 67))
            self.assertEqual((c['allocations'], c['uploads'], c['handles_created'], c['reservations'], c['rng_draws']),
                             (0, 0, 0, 0, 0))

    def test_startup_survivors_only(self):
        for r in mod.ROUTES:
            fs = DOC['routes'][r]['final_state']
            self.assertEqual(sorted(fs['references']), ['A%02d' % i for i in range(79)])
            self.assertEqual(fs['live_handles_in_order'], list(range(36)))
            self.assertEqual({k: v for k, v in TITLE['final_state']['references'].items() if k in fs['references']},
                             fs['references'])
            self.assertEqual(sorted(fs['freed_allocations'], key=lambda a: int(a[1:])),
                             ['A%d' % i for i in range(83, 166)])
            self.assertEqual(fs['hash_live_entries'], [])

    def test_retirement_order(self):
        for r in mod.ROUTES:
            hs = [e['handle'] for e in events(r, 'handle_retire')]
            self.assertEqual(hs, list(range(44, 111)) + list(range(36, 44)))
            fr = DOC['routes'][r]['final_state']['freed_allocations']
            self.assertEqual(fr[:2], ['A99', 'A100'])
            self.assertEqual(fr[-4:], ['A95', 'A96', 'A97', 'A98'])     # sp then hanten: image then CLUT
            self.assertEqual(DOC['routes'][r]['final_state']['free_record_tail'],
                             list(range(46, 113)) + list(range(38, 46)))
            kinds = [e['kind'] for e in DOC['routes'][r]['events']]
            self.assertLess(kinds.index('binding_unlink'), kinds.index('image_list_retire'))
            self.assertLess(kinds.index('image_list_reset'), kinds.index('audio_request'))
            self.assertEqual([e['node'] for e in events(r, 'node_free')], ['resource 0x221980', 'mode8'])

    def test_duplicate_names_and_hash(self):
        ev = DOC['routes']['restore']['events']
        hsteps = {e['record']: e for e in ev if e['kind'] in ('hash_tombstone', 'hash_untouched')}
        for older, newer, slot in ((62, 76, 1015), (54, 77, 23), (79, 80, 49), (81, 86, 63), (47, 112, 30)):
            self.assertEqual(hsteps[older]['kind'], 'hash_untouched')
            self.assertEqual((hsteps[newer]['kind'], hsteps[newer]['slot']), ('hash_tombstone', slot))
        names = [e['name'] for e in ev if e['kind'] == 'handle_retire']
        self.assertEqual([n for n in names if n.lower() == '5wod_a03.xim'], ['5wod_A03.xim', '5wod_a03.xim'])

    def test_effect106_is_descriptor_only(self):
        for r in mod.ROUTES:
            b = events(r, 'effect_bind_texture')
            self.assertEqual(len(b), 1)
            self.assertEqual(b[0]['handle'], 43)
            first_retire = min(e['seq'] for e in events(r, 'handle_retire'))
            for e in DOC['routes'][r]['events'][:first_retire]:
                self.assertNotIn(e['kind'], ('ref_adjust', 'free', 'handle_retire'))
            self.assertEqual([e['pc'] for e in events(r, 'effect_removal_request')], ['0x12a26c', '0x101248'])
        self.assertEqual(DOC['effect106']['efd']['parameters'], [32.0, 32.0, 3.0, 1.0])

    def test_routes_modes_and_inputs(self):
        self.assertEqual(DOC['routes']['restore']['mode'], 10)
        self.assertEqual(DOC['routes']['new_game']['mode'], 9)
        self.assertEqual(events('restore', 'pad')[-1]['selection'], 1)
        self.assertEqual([e.get('direction') for e in events('new_game', 'pad')], ['left', None])
        self.assertFalse([e for e in events('restore', 'call') if e['callee'] in ('0x124f10', '0x1251d8')])
        for r, m in (('restore', 10), ('new_game', 9)):
            stop = DOC['routes'][r]['events'][-1]
            self.assertEqual((stop['kind'], stop['pc'], stop['mode']), ('stop', '0x124548', m))

    def test_rng_has_no_reached_draw(self):
        self.assertEqual(DOC['rng']['draws_reached'], [])
        self.assertTrue(all(s['status'] == 'unreached' for s in DOC['rng']['sites_in_closure_not_reached']))

    def test_timeline(self):
        t = mod.timeline('restore', 2, 83)
        self.assertEqual((t['stable_menu'], t['scroll'], t['successor_entry'] - t['removal_request']), (83, [84, 176], 3))
        self.assertEqual(mod.scroll_frames('restore'), 93)
        self.assertEqual(mod.scroll_frames('new_game'), 94)
        self.assertEqual(mod.fade_in_frames(), 334)
        self.assertEqual((mod.phase60_frames(1), mod.phase60_frames(1, False)), (40, 41))
        self.assertLess(mod.phase60_frames(mod.fade_value_after(83)), 40)
        with self.assertRaises(ValueError):
            mod.timeline('restore', 1503, 2000)
        with self.assertRaises(ValueError):
            mod.timeline('new_game', 10, 90)

    def test_round_toward_zero(self):
        third = mod.round_f32(mod.Fraction(1, 3))
        self.assertLess(third, mod.Fraction(1, 3))
        self.assertEqual(mod.round_f32(mod.Fraction(-1, 3)), -third)
        self.assertEqual(mod.round_f32(mod.f32(0x3f800000)), 1)

class NegativeControls(unittest.TestCase):
    def fails(self, doc, needle=None):
        probs = mod.check(doc, TITLE)
        self.assertTrue(probs, 'mutation was accepted')
        if needle:
            self.assertTrue(any(needle in p for p in probs), probs)

    def mutate(self, route, fn):
        d = copy.deepcopy(DOC)
        fn(d['routes'][route]['events'])
        return renumber(d, route)

    def test_reordered_binding_retirement(self):
        def swap(ev):
            idx = [i for i, e in enumerate(ev) if e['kind'] == 'binding_retire'][:2]
            a, b = idx
            blk_a = ev[a:b]
            nxt = next(i for i in range(b + 1, len(ev)) if ev[i]['kind'] in ('binding_retire', 'mesh_slot_free'))
            blk_b = ev[b:nxt]
            ev[a:nxt] = blk_b + blk_a
        self.fails(self.mutate('restore', swap))

    def test_missing_free(self):
        def drop(ev):
            i = next(i for i, e in enumerate(ev) if e['kind'] == 'free')
            del ev[i]
        self.fails(self.mutate('new_game', drop))

    def test_changed_binding_handle(self):
        self.fails(self.mutate('restore', lambda ev: next(e for e in ev if e['kind'] == 'effect_bind_texture')
                               .update(handle=110)), 'hanten')

    def test_invented_refcount_on_rebind(self):
        def add(ev):
            i = next(i for i, e in enumerate(ev) if e['kind'] == 'effect_bind_texture')
            ev.insert(i + 1, {'checkpoint': 'first-update', 'kind': 'ref_adjust', 'status': 'proven', 'pc': '0x10c208',
                              'alloc': 'A97', 'delta': 1, 'references_after': 2})
        self.fails(self.mutate('restore', add), 'outside retirement')

    def test_duplicated_retire(self):
        def dup(ev):
            i = next(i for i, e in enumerate(ev) if e['kind'] == 'handle_retire')
            ev.insert(i, copy.deepcopy(ev[i]))
        self.fails(self.mutate('restore', dup))

    def test_truncated_checkpoints(self):
        def cut(ev):
            i = next(i for i, e in enumerate(ev) if e['kind'] == 'checkpoint' and e['name'] == 'successor-entry')
            del ev[i:]
        self.fails(self.mutate('restore', cut), 'checkpoint order')

    def test_duplicated_checkpoint(self):
        def dup(ev):
            i = next(i for i, e in enumerate(ev) if e['kind'] == 'checkpoint' and e['name'] == 'stable-menu')
            ev.insert(i, copy.deepcopy(ev[i]))
        self.fails(self.mutate('new_game', dup), 'checkpoint order')

    def test_swapped_mode(self):
        d = copy.deepcopy(DOC)
        d['routes']['restore']['events'][-1]['mode'] = 9
        self.fails(d, '0x124548')

    def test_new_game_without_left(self):
        d = self.mutate('new_game', lambda ev: ev.remove(next(e for e in ev if e.get('direction') == 'left')))
        self.fails(d, 'left')

    def test_invented_rng_draw(self):
        d = copy.deepcopy(DOC)
        d['rng']['draws_reached'] = [{'site': '0x10c8c4'}]
        self.fails(d, 'rng')

    def test_stale_final_state(self):
        d = copy.deepcopy(DOC)
        d['routes']['restore']['final_state']['references']['A97'] = 1
        self.fails(d, 'final references')

    def test_wrong_baseline(self):
        d = copy.deepcopy(DOC)
        d['baseline']['title_sha256'] = '0' * 64
        self.assertTrue(mod.check(d))

    def test_empty_and_self(self):
        self.assertTrue(mod.check({}, TITLE))
        d = copy.deepcopy(DOC)
        d['routes']['restore']['events'] = []
        self.fails(d)

    def test_malformed_efd_facts(self):
        d = copy.deepcopy(DOC)
        d['assets']['tes_efd_106']['parameters'][3] = 2.0
        self.fails(d, 'EFD')

@unittest.skipUnless(local_root(), 'owned local data (extracted/SCUS_971.29) not present')
class Regenerate(unittest.TestCase):
    def test_regenerated_contract_is_byte_identical(self):
        with tempfile.TemporaryDirectory() as t:
            out = os.path.join(t, 'c.json')
            subprocess.run([sys.executable, str(ROOT / 'tools/retire_title_contract.py'), '--local', local_root(),
                            '--out', out], check=True)
            self.assertEqual(Path(out).read_bytes(), (ROOT / 'docs/retire-title-contract.json').read_bytes())

    def test_elf_anchors(self):
        elf = mod.title.Elf(os.path.join(local_root(), 'extracted/SCUS_971.29'))
        self.assertEqual(mod.verify_elf(elf), [])

if __name__ == '__main__':
    unittest.main()
