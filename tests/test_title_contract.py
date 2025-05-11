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
spec = importlib.util.spec_from_file_location('title_contract', ROOT / 'tools/title_contract.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
DOC = json.loads((ROOT / 'docs/title-contract.json').read_text())

def events(kind, doc=DOC):
    return [e for e in doc['events'] if e['kind'] == kind]

class Contract(unittest.TestCase):
    def test_committed_contract_is_valid(self):
        self.assertEqual(mod.check(DOC), [])

    def test_counts(self):
        c = DOC['counts']
        self.assertEqual((c['allocations'], c['uploads'], c['handles_created'], c['palette_binds']), (87, 87, 77, 10))
        self.assertEqual((c['handles_retired'], c['records_destroyed'], c['frees']), (2, 2, 4))
        self.assertEqual((c['reservations'], c['names_registered'], c['bindings'], c['image_list_appends']), (1, 67, 1, 10))

    def test_window_bounds(self):
        self.assertEqual(DOC['events'][0]['pc'], '0x100618')
        self.assertEqual(DOC['events'][-1]['kind'], 'stop')
        last_ret = [e for e in DOC['events'] if e['kind'] == 'return'][-1]
        self.assertEqual((last_ret['pc'], last_ret['callee']), ('0x12c8b4', '0x114c80'))
        self.assertNotIn('0x1296f8', [e.get('callee') for e in DOC['events']])

    def test_constructor_order(self):
        sel = [e['selector'] for e in events('construct') if e.get('mode') == 8]
        self.assertEqual(sel, [3, 0, 1])
        req = [e['selection'] for e in events('request')]
        self.assertEqual(req, [[8, 0, 0], [8, 1, 0]])
        dt = [(e['node'], e['selector']) for e in events('destructor')]
        self.assertEqual(dt, [('resource 0x221980', 3), ('mode8', 3), ('resource 0x221980', 0), ('mode8', 0)])

    def test_logo_retirement_in_insertion_order(self):
        logo = [e['handle'] for e in events('image_list_append')][:2]
        retired = [e['handle'] for e in events('image_list_retire')]
        self.assertEqual(retired, logo)
        self.assertEqual(DOC['final_state']['freed_allocations'], ['A79', 'A80', 'A81', 'A82'])
        seq = {e['seq']: e for e in DOC['events']}
        first_title = next(e for e in events('image_load') if e['name'] == 'title_sample4.xim')['seq']
        self.assertTrue(all(e['seq'] < first_title for e in events('free')))

    def test_title_images_then_night_v(self):
        names = [e['name'] for e in events('image_load')]
        self.assertEqual(names, ['scea.xim', 'zener.xim', 'title_sample4.xim', 'push.xim', 'new.xim', 'restore.xim',
                                 'maru_w.xim', 'maru_b.xim', 'sp.xim', 'hanten.xim'])
        last_img = max(e['seq'] for e in events('image_load'))
        self.assertTrue(all(e['seq'] > last_img for e in events('xmd_name')))
        self.assertEqual(DOC['final_state']['mode8_0xcc_handle'], 43)

    def test_scr_formats(self):
        for i in DOC['assets']['logos'] + DOC['assets']['title_images']:
            self.assertEqual((i['psm'], i['cpsm'], i['levels'], i['colors']), (19, 0, 1, 256))
            self.assertEqual(i['tail_bytes'], 0)

    def test_night_v_table(self):
        x = DOC['assets']['night_v_xmd']
        self.assertEqual((x['names'], x['distinct_case_insensitive'], x['meshes']), (67, 62, 1))
        self.assertEqual(sorted(x['duplicate_groups']), [[1, 66], [8, 31], [16, 30], [33, 34], [35, 40]])
        tex = DOC['assets']['night_v_textures']
        self.assertTrue(all(t['psm'] == 1 and t['levels'] == 0 for t in tex))
        self.assertEqual(tex[0]['authored'], '5wod_g00')
        self.assertEqual(tex[66]['authored'], '5wod_a03')
        self.assertEqual(tex[1]['authored'], '5wod_A03')
        self.assertEqual(tex[1]['sha256'], tex[66]['sha256'])

    def test_class_cache_and_binding(self):
        cs = events('cache_state')[0]
        self.assertEqual((cs['cache_enabled'], cs['lower_case_file_names'], cs['reserved_class']), (0, 0, 0))
        r = events('reserve')[0]
        self.assertEqual((r['reserved_class'], r['count'], r['first']), (0, 67, 44))
        b = events('xmd_binding')[0]
        self.assertEqual(b['handles'], list(range(44, 111)))
        loaded = [e['handle'] for e in events('handle_create') if e['from_reserved_queue']]
        self.assertEqual(b['handles'], loaded)
        self.assertNotIn('0x1cce78', [e.get('callee') for e in DOC['events']])

    def test_duplicates_newest_wins(self):
        d = {e['record'] - 46: e['displaced_record'] for e in events('name_register') if e['displaced_record'] is not None}
        self.assertEqual({k: v - 46 for k, v in d.items()}, {30: 16, 31: 8, 34: 33, 40: 35, 66: 1})

    def test_gs_writer_regions(self):
        w = {g['id']: g for g in DOC['gs_writers']}
        self.assertEqual({r['alloc'] for r in w['W1']['regions']}, {'A00', 'A01', 'A02'})
        self.assertEqual({r['alloc'] for r in w['W5']['regions']}, {'A08', 'A09', 'A10'})
        self.assertTrue(all(r['status'] == 'unwritten' for r in w['W5']['regions']))
        self.assertEqual(w['W3']['tag'], 'U')

class Negative(unittest.TestCase):
    def mutated(self, fn):
        d = copy.deepcopy(DOC)
        fn(d)
        return mod.check(d)

    def test_first_plus_i_is_not_successor_when_list_has_holes(self):
        def f(d):
            b = next(e for e in d['events'] if e['kind'] == 'xmd_binding')
            b['handles'][3] = 99
        self.assertTrue(self.mutated(f))

    def test_retire_order_swapped(self):
        def f(d):
            r = [e for e in d['events'] if e['kind'] == 'image_list_retire']
            r[0]['handle'], r[1]['handle'] = r[1]['handle'], r[0]['handle']
        self.assertTrue(self.mutated(f))

    def test_reserved_class_one_rejected(self):
        self.assertTrue(self.mutated(lambda d: next(e for e in d['events'] if e['kind'] == 'reserve').update(reserved_class=1)))

    def test_missing_free(self):
        def f(d):
            i = next(e['seq'] for e in d['events'] if e['kind'] == 'free')
            d['events'].pop(i)
            for k, e in enumerate(d['events']):
                e['seq'] = k
        self.assertTrue(self.mutated(f))

    def test_hash_slot(self):
        self.assertTrue(self.mutated(lambda d: next(e for e in d['events'] if e['kind'] == 'name_register').update(hash_slot=5)))

    def test_image_record_handle(self):
        self.assertTrue(self.mutated(lambda d: next(e for e in d['events'] if e['kind'] == 'image_record').update(handle=0)))

    def test_handle_id(self):
        self.assertTrue(self.mutated(lambda d: next(e for e in d['events'] if e['kind'] == 'handle_create').update(handle=50)))

    def test_upload_qwc(self):
        def f(d):
            u = next(e for e in d['events'] if e['kind'] == 'upload' and e['psm'] == 1)
            u['qwc'] += 1
            u['bytes'] += 16
        self.assertTrue(self.mutated(f))

    def test_stop_must_be_last(self):
        self.assertTrue(self.mutated(lambda d: d['events'].append(dict(d['events'][0], seq=len(d['events'])))))

    def test_counts(self):
        self.assertTrue(self.mutated(lambda d: d['counts'].update(frees=3)))

    def test_title_order(self):
        def f(d):
            t = d['assets']['title_images']
            t[1], t[2] = t[2], t[1]
        self.assertTrue(self.mutated(f))

    def test_psm_table(self):
        self.assertTrue(self.mutated(lambda d: d['assets']['night_v_textures'][5].update(psm=0)))

    def test_cache_enabled(self):
        self.assertTrue(self.mutated(lambda d: next(e for e in d['events'] if e['kind'] == 'cache_state').update(cache_enabled=1)))

    def test_unresolved_required(self):
        self.assertTrue(self.mutated(lambda d: d.update(unresolved=[])))

class Primitives(unittest.TestCase):
    def test_psm1_qwc_floor(self):
        self.assertEqual(mod.qwc(1, 64, 64), 768)
        self.assertEqual(mod.qwc(1, 5, 3), (15 * 3) >> 4)
        self.assertEqual(mod.qwc(19, 640, 480), 19200)
        self.assertEqual(mod.clut_rows(256), 16)
        self.assertEqual(mod.clut_rows(16), 2)

    def test_successor_traversal_with_hole_and_end(self):
        m = mod.HandleModel([0, 1, 2], [0, 1, 2])
        m.reserve(4, 0)
        hs = [m.create(0)[0] for _ in range(4)]
        self.assertEqual(hs, [3, 4, 5, 6])
        m.retire(4)                                   # hole
        self.assertEqual(mod.xmd_binding(m, 3, 4), [3, 5, 6, mod.END_OF_LIST])
        self.assertEqual(mod.xmd_binding(m, 3, 5)[-1], -1)
        self.assertEqual(mod.xmd_binding(m, -1, 4), [])

    def test_failed_load_shifts_slots(self):
        m = mod.HandleModel([], [])
        m.reserve(3, 0)
        a = m.create(0)[0]
        # second name fails (no handle consumed), third consumes the next reserved id
        c = m.create(0)[0]
        self.assertEqual((a, c), (0, 1))
        self.assertEqual(mod.xmd_binding(m, a, 3), [0, 1, mod.END_OF_LIST])
        self.assertEqual(m.queue[0], [2])            # unused reservation stays queued

    def test_reserve_releases_previous_queue(self):
        m = mod.HandleModel([], [])
        self.assertEqual(m.reserve(3, 0), 0)
        self.assertEqual(m.reserve(2, 0), 0)          # ids 0..2 released first, first-fit again
        self.assertEqual(m.queue[0], [0, 1])

    def test_record_fifo_reuse(self):
        m = mod.HandleModel([0], [0])
        h1, r1 = m.create(0)
        h2, r2 = m.create(0)
        m.retire(h1)
        m.retire(h2)
        self.assertEqual(m.create(0), (1, 3))

    def test_name_hash_duplicate_and_tombstone(self):
        n = mod.NameHash()
        s1, d1 = n.register(10, 'ABC.xim')
        s2, d2 = n.register(11, 'abc.xim')
        self.assertEqual((s1, d1, s2, d2), (s1, None, s1, 10))
        self.assertEqual(n.slot[10], -1)
        self.assertEqual(n.table[s1], 11 + 8)
        self.assertEqual(mod.stored_name('A' * 30), 'a' * 19)

    def test_xmd_name_parser(self):
        b = (16).to_bytes(4, 'little') + (8).to_bytes(4, 'little') + (2).to_bytes(4, 'little') + b'ab\0Cd\0'
        self.assertEqual(mod.xmd_names(b[:4] + b[4:]), ['ab', 'Cd'])
        self.assertEqual(mod.xmd_names((0).to_bytes(8, 'little')), [])

    def test_parse_psm1_xim(self):
        w, h = 5, 3
        hdr = (1 << 20).to_bytes(8, 'little') + bytes(8)
        blk = bytes(8) + h.to_bytes(4, 'little') + w.to_bytes(4, 'little')
        pix = bytes(range(w * h * 3))
        x = mod.parse_xim(hdr + blk + pix)
        self.assertEqual((x['psm'], x['levels'], x['width'], x['height'], x['pixel_offset'], x['qwc']), (1, 0, 5, 3, 32, 2))
        self.assertEqual(x['pixels'], pix[:32])

@unittest.skipUnless(os.path.isfile(os.environ.get('OKAGE_LOCAL', str(ROOT / 'local')) + '/extracted/SCUS_971.29'),
                     'owned local data not present')
class Regenerate(unittest.TestCase):
    def test_regenerated_contract_matches(self):
        local = os.environ.get('OKAGE_LOCAL', str(ROOT / 'local'))
        with tempfile.TemporaryDirectory() as t:
            out = os.path.join(t, 'c.json')
            subprocess.run([sys.executable, str(ROOT / 'tools/title_contract.py'), '--local', local, '--out', out],
                           check=True)
            self.assertEqual(json.load(open(out)), DOC)

if __name__ == '__main__':
    unittest.main()
