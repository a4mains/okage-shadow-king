"""Controls for tools/rng_compare.py (synthetic fixtures; optional checks of the ignored RNG evidence)."""
import copy
import io
import json
import os
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import rng_compare as cmp  # noqa: E402
import rng_original_extract as ext  # noqa: E402

MAIN = Path(os.environ.get('OKAGE_MAIN_CHECKOUT', ROOT))
EVIDENCE = MAIN / 'local/research/rng-original-v1/analysis'

def h(x):
    return '0x%08x' % struct.unpack('<I', struct.pack('<f', x))[0]

def lcg_draws(counts, state=1):
    out = []
    for phase, n in counts:
        for _ in range(n):
            state = (state * 0x41c64e6d + 0x3039) & 0xffffffff
            out.append(dict(i=len(out), phase=phase, state=state, value=state & 0x7fffffff))
    return out

def quad(x0, y0):
    """Two triangles of one cell, (1,0,2),(0,2,3) corner order, white, no tags."""
    p = [(x0 + 1, y0, 1.0), (x0, y0, 1.0), (x0, y0 + 1, 1.0), (x0 + 1, y0 + 1, 1.0)]
    return [[h(p[i][0]), h(p[i][1]), h(p[i][2]), h(1.0), h(255.0), h(255.0), h(255.0), h(255.0),
             h(0.0), h(0.0), h(0.0), h(0.0)] for i in (1, 0, 2, 0, 2, 3)]

def fixture():
    draws = lcg_draws((('noise', 6), ('effect-grid', 384), ('effect-template', 4)))
    grid = [d['value'] % 7 for d in draws if d['phase'] == 'effect-grid']
    noise = quad(0.0, 0.0) + quad(1.0, 0.0)
    rays = quad(-1.0, 2.0)
    templates = [
        dict(shape=55, owner=[116, 1], order=0, draws=4, vertices=rays, position_tags=[0, 1, 2, 0, 2, 3],
             uv_tags=[0] * 6),
        dict(shape=11, owner=[99, 2], order=1, draws=0, vertices=noise, position_tags=[0] * 12, uv_tags=[0] * 12),
    ]
    effects = dict(grid=grid, templates=templates, cache_owners=[[11, 99, 2], [55, 9, 26]],
                   requests=[{'def': 116, 'elem': 1, 'shape': 55, 'constructed': True},
                             {'def': 99, 'elem': 2, 'shape': 11, 'constructed': True},
                             {'def': 9, 'elem': 26, 'shape': 55, 'constructed': False}])
    return draws, effects

def write_dir(path, draws, effects, raw_rng=None, raw_effects=None):
    path.mkdir()
    (path / 'rng.jsonl').write_text(raw_rng if raw_rng is not None else
                                    ''.join(json.dumps(d) + '\n' for d in draws))
    (path / 'effects.json').write_text(raw_effects if raw_effects is not None else json.dumps(effects))

class CompareControls(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.draws, self.effects = fixture()
        write_dir(self.base / 'orig', self.draws, self.effects)
        self.n = 0

    def run_native(self, draws=None, effects=None, raw_rng=None, raw_effects=None, extra=()):
        self.n += 1
        nat = self.base / f'native{self.n}'
        write_dir(nat, self.draws if draws is None else draws, self.effects if effects is None else effects,
                  raw_rng, raw_effects)
        out, err = io.StringIO(), io.StringIO()
        report = self.base / f'report{self.n}.json'
        with redirect_stdout(out), redirect_stderr(err):
            code = cmp.main([str(self.base / 'orig'), str(nat), '--json', str(report), *extra], _acceptance=False)
        data = json.loads(report.read_text()) if report.exists() else None
        return code, out.getvalue(), err.getvalue(), data

    def test_identical(self):
        code, out, _, r = self.run_native()
        self.assertEqual(code, 0, out)
        self.assertTrue(r['identical'])
        self.assertTrue(r['native_consistency']['grid_is_value_mod_7_of_grid_draws'])

    def test_mutated_value(self):
        d = copy.deepcopy(self.draws)
        d[100]['value'] ^= 1
        code, out, _, r = self.run_native(draws=d)
        self.assertEqual(code, 1)
        self.assertEqual(r['sections']['draws']['first']['i'], 100)
        self.assertEqual(r['sections']['draws']['first']['fields'], ['value'])
        self.assertEqual(r['sections']['draws']['native_value_not_state_masked']['first'], 100)

    def test_mutated_state_and_value(self):
        d = copy.deepcopy(self.draws)
        d[7]['state'] ^= 0x10
        d[7]['value'] = d[7]['state'] & 0x7fffffff
        code, _, _, r = self.run_native(draws=d)
        self.assertEqual(code, 1)
        self.assertEqual(r['sections']['draws']['first']['fields'], ['state', 'value'])

    def test_phase_boundary_shift(self):
        d = copy.deepcopy(self.draws)
        d[6]['phase'] = 'noise'
        code, out, _, r = self.run_native(draws=d)
        self.assertEqual(code, 1)
        self.assertFalse(r['sections']['phase_boundaries']['identical'])
        self.assertEqual(r['sections']['draws']['first']['i'], 6)
        self.assertEqual(r['sections']['draws']['first']['fields'], ['phase'])
        self.assertIn('noise[0..6]', out)

    def test_extra_and_missing_draw(self):
        extra = lcg_draws((('noise', 6), ('effect-grid', 384), ('effect-template', 5)))
        code, _, _, r = self.run_native(draws=extra)
        self.assertEqual(code, 1)
        self.assertEqual(r['sections']['draws']['length']['extra_in_native'], 1)
        self.assertEqual(r['sections']['draws']['length']['first_unmatched_index'], len(self.draws))
        code, _, _, r = self.run_native(draws=self.draws[:-1])
        self.assertEqual(code, 1)
        self.assertEqual(r['sections']['draws']['length']['missing_in_native'], 1)

    def test_swapped_templates(self):
        e = copy.deepcopy(self.effects)
        e['templates'][0]['order'], e['templates'][1]['order'] = 1, 0
        code, _, _, r = self.run_native(effects=e)
        self.assertEqual(code, 1)
        first = r['sections']['templates']['first']
        self.assertEqual(first['order'], 0)
        self.assertIn('shape', first['fields'])

    def test_flipped_float_bit(self):
        e = copy.deepcopy(self.effects)
        v = e['templates'][0]['vertices'][3]
        v[1] = '0x%08x' % (int(v[1], 16) ^ 1)
        code, _, _, r = self.run_native(effects=e)
        self.assertEqual(code, 1)
        first = r['sections']['templates']['first']
        self.assertEqual(first['first_vertex_difference']['vertex'], 3)
        self.assertEqual(first['first_vertex_difference']['components'], [1])
        self.assertFalse(first['differs_only_by_signed_zero'])

    def test_signed_zero_is_a_difference_but_flagged(self):
        e = copy.deepcopy(self.effects)
        verts = e['templates'][0]['vertices']
        k = next(j for j, v in enumerate(verts) if v[2] == '0x3f800000')
        z = next(j for j, v in enumerate(verts) if v[8] == '0x00000000')
        verts[z][8] = '0x80000000'
        code, _, _, r = self.run_native(effects=e)
        self.assertEqual(code, 1)
        self.assertTrue(r['sections']['templates']['first']['differs_only_by_signed_zero'])
        self.assertGreaterEqual(k, 0)

    def test_tag_difference(self):
        e = copy.deepcopy(self.effects)
        e['templates'][0]['position_tags'][2] = 9
        code, _, _, r = self.run_native(effects=e)
        self.assertEqual(code, 1)
        self.assertEqual(r['sections']['templates']['first']['first_vertex_difference']['vertex'], 2)

    def test_noise_triangle_order_relaxation_is_explicit_and_scoped(self):
        e = copy.deepcopy(self.effects)
        t = e['templates'][1]
        t['vertices'] = t['vertices'][6:] + t['vertices'][:6]       # same triangles, other cell order
        code, _, _, r = self.run_native(effects=e)
        self.assertEqual(code, 1)
        self.assertTrue(r['sections']['templates']['first']['same_winding_preserving_triangle_multiset'])
        code, out, _, r = self.run_native(effects=e, extra=('--noise-triangle-order', 'multiset'))
        self.assertEqual(code, 0, out)
        self.assertEqual(len(r['relaxations_applied']), 1)
        # Reversed winding is not the same triangle set, even with the relaxation.
        e2 = copy.deepcopy(self.effects)
        v = e2['templates'][1]['vertices']
        v[0], v[1] = v[1], v[0]
        code, _, _, _ = self.run_native(effects=e2, extra=('--noise-triangle-order', 'multiset'))
        self.assertEqual(code, 1)
        # A non-noise shape is never relaxed.
        e3 = copy.deepcopy(self.effects)
        t3 = e3['templates'][0]
        t3['vertices'] = t3['vertices'][3:] + t3['vertices'][:3]
        t3['position_tags'] = t3['position_tags'][3:] + t3['position_tags'][:3]
        code, _, _, _ = self.run_native(effects=e3, extra=('--noise-triangle-order', 'multiset'))
        self.assertEqual(code, 1)

    def test_grid_cache_and_request_differences(self):
        e = copy.deepcopy(self.effects)
        e['grid'][130] = (e['grid'][130] + 1) % 7
        code, _, _, r = self.run_native(effects=e)
        self.assertEqual(code, 1)
        self.assertEqual(r['sections']['grid']['first']['cell'], 130)
        self.assertEqual(r['sections']['grid']['first']['face'], 2)
        e = copy.deepcopy(self.effects)
        e['cache_owners'][1] = [55, 116, 1]
        code, _, _, r = self.run_native(effects=e)
        self.assertEqual(code, 1)
        self.assertEqual(r['sections']['cache_owners']['first']['shape'], 55)
        e = copy.deepcopy(self.effects)
        e['requests'][2]['constructed'] = True
        code, _, _, r = self.run_native(effects=e)
        self.assertEqual(code, 1)
        self.assertEqual(r['sections']['requests']['first']['index'], 2)
        e = copy.deepcopy(self.effects)
        del e['requests'][1]
        code, _, _, r = self.run_native(effects=e)
        self.assertEqual(code, 1)
        self.assertEqual(r['sections']['requests']['native_count'], 2)

    def assertMalformed(self, **kw):
        code, _, err, r = self.run_native(**kw)
        self.assertEqual(code, 2, err)
        self.assertIn('MALFORMED', err)
        self.assertIsNone(r)

    def test_duplicate_keys(self):
        lines = [json.dumps(d) for d in self.draws]
        lines[3] = lines[3][:-1] + ', "state": 5}'
        self.assertMalformed(raw_rng='\n'.join(lines) + '\n')
        text = json.dumps(self.effects)
        self.assertMalformed(raw_effects=text[:-1] + ', "grid": []}')

    def test_empty_inputs(self):
        self.assertMalformed(raw_rng='')
        self.assertMalformed(raw_effects='')
        self.assertMalformed(raw_effects='   \n')

    def test_schema_violations(self):
        d = copy.deepcopy(self.draws)
        d[2]['i'] = 5
        self.assertMalformed(draws=d)
        d = copy.deepcopy(self.draws)
        d[2]['phase'] = 'fonts'
        self.assertMalformed(draws=d)
        d = copy.deepcopy(self.draws)
        d[2]['extra'] = 1
        self.assertMalformed(draws=d)
        d = copy.deepcopy(self.draws)
        d[2]['state'] = True
        self.assertMalformed(draws=d)
        d = copy.deepcopy(self.draws)
        d[2]['value'] = 1 << 32
        self.assertMalformed(draws=d)
        self.assertMalformed(raw_rng=''.join(json.dumps(x) + '\n' for x in self.draws[:3]) + '\n')
        self.assertMalformed(raw_rng=''.join(json.dumps(x) + '\n' for x in self.draws)[:-1])
        self.assertMalformed(raw_rng=json.dumps(self.draws[0]).replace('1103527590', 'NaN') + '\n')
        e = copy.deepcopy(self.effects)
        e['grid'] = e['grid'][:-1]
        self.assertMalformed(effects=e)
        e = copy.deepcopy(self.effects)
        e['templates'][0]['vertices'][0][0] = '0x3F800000'
        self.assertMalformed(effects=e)
        e = copy.deepcopy(self.effects)
        e['templates'][0]['vertices'][0] = e['templates'][0]['vertices'][0][:11]
        self.assertMalformed(effects=e)
        e = copy.deepcopy(self.effects)
        e['templates'][0]['uv_tags'] = e['templates'][0]['uv_tags'][:-1]
        self.assertMalformed(effects=e)
        e = copy.deepcopy(self.effects)
        e['templates'][1]['order'] = 0
        self.assertMalformed(effects=e)
        e = copy.deepcopy(self.effects)
        e['cache_owners'].append([11, 1, 1])
        self.assertMalformed(effects=e)
        e = copy.deepcopy(self.effects)
        e['cache_owners'].append([20, 0, 0])
        self.assertMalformed(effects=e)
        e = copy.deepcopy(self.effects)
        e['requests'][0]['constructed'] = 1
        self.assertMalformed(effects=e)
        e = copy.deepcopy(self.effects)
        del e['requests']
        self.assertMalformed(effects=e)

    def test_missing_directory_and_overwrite(self):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            self.assertEqual(cmp.main([str(self.base / 'orig'), str(self.base / 'absent')]), 2)
        existing = self.base / 'exists.json'
        existing.write_text('{}')
        nat = self.base / 'nat-ow'
        write_dir(nat, self.draws, self.effects)
        with redirect_stdout(out), redirect_stderr(err):
            self.assertEqual(cmp.main([str(self.base / 'orig'), str(nat), '--json', str(existing)], _acceptance=False), 2)
        self.assertEqual(existing.read_text(), '{}')

class ExtractorUnits(unittest.TestCase):
    def test_f32hex_is_exact(self):
        self.assertEqual(ext.f32hex(1.0), '0x3f800000')
        self.assertEqual(ext.f32hex(-1 / 64), '0xbc800000')
        with self.assertRaises(ext.ExtractError):
            ext.f32hex(0.1)

    def test_unpack_records(self):
        payload = struct.pack('<3h3h', 64, -64, 0, 1, 2, 3)
        body = struct.pack('<I', (0x79 << 24) | (2 << 16) | 0x8001) + payload
        body += b'\0' * ((-len(body)) % 16)
        qwc = len(body) // 16
        pkt = struct.pack('<4I', qwc, 0, 0, 0) + body + b'\0' * 16
        self.assertEqual(ext.unpack_records(pkt, 0x79, '<3h'), [[(64, -64, 0), (1, 2, 3)]])
        bad = bytearray(pkt)
        struct.pack_into('<I', bad, 16, (0x79 << 24) | (2 << 16) | 0x8002)
        with self.assertRaises(ext.ExtractError):
            ext.unpack_records(bytes(bad), 0x79, '<3h')
        with self.assertRaises(ext.ExtractError):
            ext.unpack_records(pkt[:-16], 0x79, '<3h')

@unittest.skipUnless((EVIDENCE / 'rng.jsonl').is_file(), 'RNG original evidence absent (set OKAGE_MAIN_CHECKOUT)')
class EvidenceChecks(unittest.TestCase):
    def test_reference_schema_phases_and_self_comparison(self):
        draws = cmp.load_rng(str(EVIDENCE / 'rng.jsonl'))
        fx = cmp.load_effects(str(EVIDENCE / 'effects.json'))
        self.assertEqual(len(draws), 9504)
        self.assertEqual(cmp.runs(draws), [
            dict(phase='noise', first=0, last=8959, count=8960),
            dict(phase='effect-grid', first=8960, last=9343, count=384),
            dict(phase='effect-template', first=9344, last=9503, count=160)])
        self.assertEqual((draws[8959]['state'], draws[9343]['state'], draws[9503]['state']),
                         (0xceeb6501, 0x728e2781, 0x412c25e1))
        self.assertEqual(sum(t['draws'] for t in fx['templates']), 160)
        r = cmp.compare(str(EVIDENCE), str(EVIDENCE))
        self.assertTrue(r['identical'])

    def cli(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            return cmp.main([str(a) for a in args])

    def test_cli_gate_refuses_self_unpinned_and_fabricated_inputs(self):
        import shutil
        self.assertEqual(self.cli(EVIDENCE, EVIDENCE), 2)  # self-comparison
        with tempfile.TemporaryDirectory() as tmp:
            tampered = Path(tmp) / 'orig'
            shutil.copytree(EVIDENCE, tampered)
            (tampered / 'rng.jsonl').chmod(0o644)
            lines = (tampered / 'rng.jsonl').read_text().splitlines()
            (tampered / 'rng.jsonl').write_text('\n'.join(lines[:1]) + '\n')
            native = Path(tmp) / 'native'
            native.mkdir()
            (native / 'rng.jsonl').write_text(lines[0] + '\n')
            shutil.copy(EVIDENCE / 'effects.json', native / 'effects.json')
            self.assertEqual(self.cli(tampered, native), 2)  # original not the pinned capture
            self.assertEqual(self.cli(EVIDENCE, native), 2)  # truncated native: totals enforced

if __name__ == '__main__':
    unittest.main()
