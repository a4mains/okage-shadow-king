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
spec = importlib.util.spec_from_file_location('restore_contract', ROOT / 'tools/restore_contract.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
DOC = json.loads((ROOT / 'docs/restore-contract.json').read_text())
TITLE = json.loads((ROOT / 'docs/title-contract.json').read_text())
RETIRE = json.loads((ROOT / 'docs/retire-title-contract.json').read_text())

def events(kind, doc=DOC):
    return [e for e in doc['events'] if e['kind'] == kind]

def renumber(doc):
    for i, e in enumerate(doc['events']):
        e['seq'] = i
    doc['counts'] = mod.counts_of(doc['events'])
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

    def test_baselines_are_pinned(self):
        self.assertEqual(DOC['baseline']['title_sha256'], mod.TITLE_SHA256)
        self.assertEqual(DOC['baseline']['retire_sha256'], mod.RETIRE_SHA256)
        s = DOC['baseline']['start_state']
        self.assertEqual(s['live_handles_in_order'], list(range(36)))
        self.assertEqual((s['live_allocations'], s['next_allocation_id']), (79, 'A166'))
        self.assertEqual(s['free_record_head'][0], 113)
        self.assertEqual(len(s['hash_tombstones']), 62)

    def test_wrapper_atlas(self):
        a = DOC['wrapper_atlas']
        self.assertEqual((a['psm'], a['cpsm'], a['width'], a['height'], a['size']), (19, 0, 256, 320, 82992))
        hc = events('handle_create')[0]
        self.assertEqual((hc['handle'], hc['record'], hc['name']), (36, 113, 'bm_if.xim'))
        self.assertEqual([e['id'] for e in events('alloc')][:2], ['A166', 'A167'])
        self.assertEqual([e['handle'] for e in events('handle_retire')], [36])
        self.assertEqual(DOC['final_state']['freed_allocations'], ['A166', 'A167'])
        kinds = [e['kind'] for e in DOC['events']]
        self.assertLess(kinds.index('window_destroy'), kinds.index('atlas_retire'))
        self.assertLess(kinds.index('atlas_retire'), kinds.index('image_load'))
        self.assertEqual(events('hash_untouched')[0]['record'], 113)

    def test_route_shape(self):
        self.assertEqual([e['name'] for e in events('checkpoint')], list(mod.CHECKPOINTS))
        self.assertEqual([e['node'] for e in events('destructor')], ['manager', 'resource 0x221980', 'wrapper'])
        self.assertEqual([e['node'] for e in events('node_free')], ['manager', 'resource 0x221980', 'wrapper'])
        mr = events('mode_request')
        self.assertEqual((mr[0]['mode'], mr[0]['selector'], mr[0]['pc']), (8, 1, '0x12452c'))
        self.assertEqual(events('death')[0]['pc'], '0x11f52c')
        self.assertEqual(events('pad')[0]['mask'][:16], '*0x1f85e0 = 0x20')
        self.assertEqual(events('effect_reclaim')[0]['frame'], 'S')
        stop = DOC['events'][-1]
        self.assertEqual((stop['kind'], stop['pc']), ('stop', '0x11528c'))

    def test_second_title_uses_live_state(self):
        st = DOC['second_title']
        self.assertEqual(st['title_handles'], list(range(36, 44)))
        self.assertEqual(events('xmd_binding')[0]['handles'], list(range(44, 111)))
        recs = [e['record'] for e in events('handle_create')[1:]]
        self.assertEqual(recs, list(range(114, 189)))           # FIFO head, not the first title's 38..112
        title_recs = [e['record'] for e in TITLE['events'] if e['kind'] == 'handle_create'][2:]
        self.assertNotEqual(recs, title_recs)
        first = {e['stored']: [] for e in TITLE['events'] if e['kind'] == 'name_register'}
        slots = [(e['stored'], e['hash_slot']) for e in events('name_register')]
        title_slots = [(e['stored'], e['hash_slot']) for e in TITLE['events'] if e['kind'] == 'name_register']
        self.assertEqual(slots, title_slots)
        self.assertEqual(len(first), 62)
        # duplicate pairs (Title 1/66, 8/31, 16/30, 33/34, 35/40) displace the older record: now 122+i
        self.assertEqual([e['displaced_record'] for e in events('name_register') if e['displaced_record'] is not None],
                         [138, 130, 155, 157, 123])
        fs = DOC['final_state']
        self.assertEqual(fs['live_handles_in_order'], list(range(111)))
        self.assertEqual(len(fs['references']), 79 + 83)
        self.assertEqual(fs['next_allocation'], 'A251')
        self.assertEqual(len(fs['hash_live_entries']), 62)

    def test_rng_and_glyph(self):
        self.assertEqual([s['site'] for s in DOC['rng']['sites']], ['0x19c74c', '0x19cad4', '0x19cc78'])
        rd = events('rng_draws')
        self.assertEqual(rd[0]['count'], 32)
        self.assertTrue(all(e['count'] == 'R(frame)' and e['status'] == 'open' for e in rd[1:]))
        self.assertTrue(DOC['glyph_cache']['verdict'].startswith('NOT reached'))
        self.assertEqual(DOC['counts']['glyph_cache_writes'], 0)

    def test_start_state_is_replayed_not_copied(self):
        st = mod.Start(TITLE, RETIRE)
        self.assertEqual(st.model.free_records[:3], [113, 114, 115])
        self.assertEqual(st.model.free_records[-8:], list(range(38, 46)))

class NegativeControls(unittest.TestCase):
    def fails(self, doc, needle=None):
        probs = mod.check(doc, TITLE, RETIRE)
        self.assertTrue(probs, 'mutation was accepted')
        if needle:
            self.assertTrue(any(needle in p for p in probs), probs)

    def mutate(self, fn):
        d = copy.deepcopy(DOC)
        fn(d['events'])
        return renumber(d)

    def test_missing_free(self):
        self.fails(self.mutate(lambda ev: ev.remove(next(e for e in ev if e['kind'] == 'free'))))

    def test_missing_checkpoint(self):
        self.fails(self.mutate(lambda ev: ev.remove(next(e for e in ev if e['kind'] == 'checkpoint'
                                                          and e['name'] == 'empty-ready'))), 'checkpoint order')

    def test_reordered_checkpoints(self):
        def swap(ev):
            i = next(i for i, e in enumerate(ev) if e.get('name') == 'resource-retirement')
            j = next(i for i, e in enumerate(ev) if e.get('name') == 'wrapper-free')
            ev[i]['name'], ev[j]['name'] = ev[j]['name'], ev[i]['name']
        self.fails(self.mutate(swap), 'checkpoint')

    def test_atlas_retired_before_window_death(self):
        def move(ev):
            i = next(i for i, e in enumerate(ev) if e['kind'] == 'atlas_retire')
            blk = ev[i:i + 9]
            del ev[i:i + 9]
            j = next(i for i, e in enumerate(ev) if e['kind'] == 'window_destroy')
            for k, e in enumerate(blk):
                ev.insert(j + k, e)
        self.fails(self.mutate(move))

    def test_changed_atlas_psm(self):
        d = self.mutate(lambda ev: next(e for e in ev if e['kind'] == 'upload').update(psm=27))
        self.fails(d, 'PSM19')

    def test_changed_atlas_bytes(self):
        d = self.mutate(lambda ev: next(e for e in ev if e['kind'] == 'upload').update(sha256='0' * 64))
        self.fails(d, 'upload bytes')
        d = self.mutate(lambda ev: [e for e in ev if e['kind'] == 'upload'][1].update(bytes=1088))
        self.fails(d, 'upload bytes')

    def test_invented_glyph_upload(self):
        def add(ev):
            i = next(i for i, e in enumerate(ev) if e['kind'] == 'rng_draws' and e['frame'] == 'S+1')
            ev.insert(i, {'checkpoint': 'first-layout', 'frame': 'S+1', 'kind': 'upload', 'status': 'proven',
                          'pc': '0x1976f0', 'alloc': 'A03', 'psm': 20})
        self.fails(self.mutate(add), 'outside')

    def test_rng_draw_omission(self):
        self.fails(self.mutate(lambda ev: ev.remove(next(e for e in ev if e['kind'] == 'rng_draws'
                                                          and e['frame'] == 'S'))), 'RNG')

    def test_rng_after_death(self):
        def add(ev):
            i = next(i for i, e in enumerate(ev) if e['kind'] == 'stop')
            ev.insert(i, {'checkpoint': 'second-title-return', 'frame': 'D+5', 'kind': 'rng_draws',
                          'status': 'proven', 'pc': '0x19c74c', 'count': 1})
        self.fails(self.mutate(add))

    def test_stale_contiguous_records(self):
        def stale(ev):
            hc = [e for e in ev if e['kind'] == 'handle_create'][1:]
            for k, e in enumerate(hc):
                e['record'] = 38 + k
        self.fails(self.mutate(stale), 'handle_create')

    def test_reset_allocator_ids(self):
        def reset(ev):
            n = 79
            for e in ev:
                if e['kind'] == 'alloc':
                    e['id'] = 'A%02d' % n
                    n += 1
        self.fails(self.mutate(reset), 'allocation id')

    def test_corrupt_hash_slot(self):
        d = self.mutate(lambda ev: next(e for e in ev if e['kind'] == 'name_register').update(hash_slot=999))
        self.fails(d, 'name hash')

    def test_wrong_mode_request(self):
        d = self.mutate(lambda ev: next(e for e in ev if e['kind'] == 'mode_request').update(selector=0))
        self.fails(d, 'mode request')

    def test_missing_effect_reclaim(self):
        self.fails(self.mutate(lambda ev: ev.remove(next(e for e in ev if e['kind'] == 'effect_reclaim'))), 'reclaim')

    def test_duplicated_retire(self):
        def dup(ev):
            i = next(i for i, e in enumerate(ev) if e['kind'] == 'handle_retire')
            ev.insert(i, copy.deepcopy(ev[i]))
        self.fails(self.mutate(dup))

    def test_glyph_verdict_flip(self):
        d = copy.deepcopy(DOC)
        d['glyph_cache']['verdict'] = 'reached'
        self.fails(d, 'glyph')

    def test_stale_final_state(self):
        d = copy.deepcopy(DOC)
        d['final_state']['freed_allocations'] = []
        self.fails(d, 'final state')

    def test_corrupt_manifest_and_empty(self):
        d = copy.deepcopy(DOC)
        d['baseline']['retire_sha256'] = '0' * 64
        self.assertTrue(mod.check(d, TITLE, RETIRE))
        self.assertTrue(mod.check({}, TITLE, RETIRE))
        d = copy.deepcopy(DOC)
        d['events'] = []
        self.fails(d)

    def test_duplicated_checkpoint(self):
        def dup(ev):
            i = next(i for i, e in enumerate(ev) if e['kind'] == 'checkpoint' and e['name'] == 'cancel-death')
            ev.insert(i, copy.deepcopy(ev[i]))
        self.fails(self.mutate(dup), 'checkpoint order')

@unittest.skipUnless(local_root(), 'owned local data (extracted/SCUS_971.29) not present')
class Regenerate(unittest.TestCase):
    def test_regenerated_contract_is_byte_identical(self):
        with tempfile.TemporaryDirectory() as t:
            out = os.path.join(t, 'c.json')
            subprocess.run([sys.executable, str(ROOT / 'tools/restore_contract.py'), '--local', local_root(),
                            '--out', out], check=True)
            self.assertEqual(Path(out).read_bytes(), (ROOT / 'docs/restore-contract.json').read_bytes())

    def test_elf_anchors(self):
        elf = mod.title.Elf(os.path.join(local_root(), 'extracted/SCUS_971.29'))
        self.assertEqual(mod.verify_elf(elf), [])

if __name__ == '__main__':
    unittest.main()
