import copy
import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[1]
spec = importlib.util.spec_from_file_location('startup_contract', ROOT / 'tools/startup_contract.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
DOC = json.loads((ROOT / 'docs/startup-contract.json').read_text())

class StartupContractSchema(unittest.TestCase):
    def test_committed_contract_is_valid(self):
        self.assertEqual(mod.check(DOC), [])

    def test_expected_counts(self):
        c = DOC['counts']
        self.assertEqual((c['allocations'], c['uploads'], c['handles'], c['palette_binds']), (79, 72, 36, 37))
        self.assertEqual(c['allocations_without_upload'], 7)
        self.assertEqual(c['frees_executed'], 0)

    def test_order_of_steps(self):
        steps = [e['step'] for e in DOC['events'] if e['kind'] == 'alloc']
        order = ['1', '1b', '2', '3', '4']
        self.assertEqual(sorted(set(steps), key=order.index), order)
        self.assertEqual(steps, sorted(steps, key=order.index))

    def test_startup_shape(self):
        al = [e for e in DOC['events'] if e['kind'] == 'alloc']
        self.assertEqual([(a['alloc_class'], a['psm'], a['width'], a['height']) for a in al[:3]],
                         [(2, 0, 640, 224), (2, 0, 640, 224), (3, 49, 640, 224)])
        eff = [a for a in al if a['step'] == '3' and a['alloc_class'] == 1]
        self.assertEqual(len(eff), 31)
        self.assertEqual(sum(a['psm'] == 20 for a in eff), 20)
        self.assertEqual(sum(a['psm'] == 19 for a in eff), 11)
        fonts = [a for a in al if a['step'] == '4' and a['alloc_class'] == 1]
        self.assertEqual([a['psm'] for a in fonts], [44, 36, 20])
        # shared depth is attached by both layers and the capture target
        self.assertEqual(DOC['final_references']['A02'], 3)
        # null-source filter CLUTs are never referenced and never uploaded
        self.assertEqual(DOC['final_references']['A09'], 0)
        up = {e['alloc'] for e in DOC['events'] if e['kind'] == 'upload'}
        self.assertNotIn('A09', up)
        self.assertNotIn('A10', up)

    def test_top_level_calls_end_at_scheduler(self):
        tc = DOC['top_level_calls']
        self.assertEqual(tc[-1]['pc'], '0x100618')
        self.assertEqual(tc[-1]['callee'], '0x114f50')
        pcs = [int(t['pc'], 16) for t in tc]
        self.assertEqual(pcs, sorted(pcs))

class NegativeControls(unittest.TestCase):
    def mutated(self, fn):
        d = copy.deepcopy(DOC)
        fn(d)
        return mod.check(d)

    def test_wrong_elf_hash(self):
        self.assertTrue(self.mutated(lambda d: d['elf'].update(sha256='0' * 64)))

    def test_duplicate_allocation_id(self):
        def f(d):
            al = [e for e in d['events'] if e['kind'] == 'alloc']
            al[1]['id'] = al[0]['id']
        self.assertTrue(self.mutated(f))

    def test_upload_rect_must_match_allocation(self):
        def f(d):
            u = next(e for e in d['events'] if e['kind'] == 'upload')
            u['rrw'] += 1
        self.assertTrue(self.mutated(f))

    def test_reference_replay_must_match(self):
        def f(d):
            r = next(e for e in d['events'] if e['kind'] == 'ref_adjust')
            r['references_after'] += 1
        self.assertTrue(self.mutated(f))

    def test_counts_must_match(self):
        self.assertTrue(self.mutated(lambda d: d['counts'].update(uploads=71)))

    def test_missing_unresolved_list(self):
        self.assertTrue(self.mutated(lambda d: d.update(unresolved=[])))

    def test_sequence_gap(self):
        self.assertTrue(self.mutated(lambda d: d['events'].pop(5)))

    def test_unknown_psm(self):
        def f(d):
            next(e for e in d['events'] if e['kind'] == 'alloc')['psm'] = 7
        self.assertTrue(self.mutated(f))

class Primitives(unittest.TestCase):
    def test_rand_matches_documented_lcg(self):
        r = mod.LibcRand(1)
        self.assertEqual(r(), (1 * 0x41c64e6d + 0x3039) & 0x7fffffff)
        self.assertEqual(r.calls, 1)

    def test_clut_swizzle_16_entries(self):
        words = list(range(100, 116))
        buf = mod.clut_swizzle(words, 16)
        self.assertEqual(buf[0:8], list(range(100, 108)))
        self.assertEqual(buf[16:24], list(range(108, 116)))
        self.assertEqual(buf[8:16], [0] * 8)
        self.assertEqual(buf[24:32], [0] * 8)
        data, height = mod.clut_upload_bytes(words, 16)
        self.assertEqual((len(data), height), (128, 2))

    def test_clut_swizzle_256_is_a_permutation(self):
        words = list(range(1, 257))
        buf = mod.clut_swizzle(words, 256)
        self.assertEqual(sorted(buf[:256]), words)
        self.assertEqual(buf[256:], [0] * 256)

    def test_font_payload_layout(self):
        count = 3
        head = bytes([3, 0, 0, 0, 0x20, 0x22]) + bytes(2)
        glyphs = bytes(0x14 * count)
        atlas = (4).to_bytes(2, 'little') + (8).to_bytes(2, 'little')   # height 4, width 8
        body = bytes(range(16))
        f = mod.font_payload(head + glyphs + atlas + body)
        self.assertEqual((f['count'], f['width'], f['height']), (3, 8, 4))
        self.assertEqual(f['payload'], body)
        self.assertEqual(f['offset'], 8 + 0x14 * 3 + 4)

if __name__ == '__main__':
    unittest.main()
