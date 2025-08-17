"""Controls for tools/restore_light_compare.py and the light extraction helpers.

SYNTHETIC fixture (the Restore comparator fixture with the original's post-successor draws turned into derived
"restore-ui"/"lcg-step" rows); plus, when the ignored evidence exists (OKAGE_MAIN_CHECKOUT), an original-derived
fixture from the pinned LIGHT extract that must compare IDENTICAL and fail under corruption.
"""
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
sys.path.insert(0, str(ROOT / 'tests'))
import restore_compare as g  # noqa: E402
import restore_light_compare as lc  # noqa: E402
import restore_original_extract as ext  # noqa: E402
import test_restore_compare as base  # noqa: E402

MAIN = Path(os.environ.get('OKAGE_MAIN_CHECKOUT', ROOT))
LIGHT = MAIN / 'local/research/restore-original-v1/light/analysis'
CPS = g.CHECKPOINTS

def lightify(rows):
    return [dict(r, phase='restore-ui', derived='lcg-step') if r['phase'].startswith('restore-ui') else r for r in rows]

class LightFixture(base.Fixture):
    def __init__(self, tmp):
        super().__init__(tmp)
        for cp in CPS:
            p = self.orig / cp / 'rng.jsonl'
            p.write_text(base.jl(lightify([json.loads(x) for x in p.read_text().splitlines()])))
        self.pin()

    def run(self, strict=False):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            code = lc.main([str(self.orig), str(self.nat), '--manifest', str(self.manifest)] + (['--strict'] if strict else []))
        return code, out.getvalue()

class LightSynthetic(unittest.TestCase):
    def setUp(self):
        self.saved = g.VRAM
        g.VRAM = base.SMALL
        self.td = tempfile.TemporaryDirectory()
        self.f = LightFixture(self.td.name)

    def tearDown(self):
        g.VRAM = self.saved
        self.td.cleanup()

    def expect(self, code, text=None):
        c, out = self.f.run()
        self.assertEqual(c, code, out)
        if text:
            self.assertIn(text, out)
        return out

    def rng_all(self, fn):
        for cp in CPS:
            rows = self.f.native_rows(cp, 'rng.jsonl')
            self.f.rewrite(cp, 'rng.jsonl', fn(rows))

    def test_native_labels_map_to_light_phase(self):
        self.expect(0, 'light-window draws compared')

    def test_full_comparator_refuses_light_rows(self):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            self.assertEqual(g.main([str(self.f.orig), str(self.f.nat), '--manifest', str(self.f.manifest)]), 2)

    def test_window_state_change_detected(self):
        def f(rows):
            for r in rows:
                if r['i'] == 2:
                    r['state'] = 10
            return rows
        self.rng_all(f)
        self.expect(1, 'rng: DIFFERENT')

    def test_window_value_change_detected(self):
        def f(rows):
            for r in rows:
                if r['i'] == 1:
                    r['value'] = 8
            return rows
        self.rng_all(f)
        self.expect(1, 'rng: DIFFERENT')

    def test_window_draw_omitted(self):
        self.rng_all(lambda rows: [r for r in rows if r['i'] != 1])
        self.expect(2)

    def test_window_tail_draw_omitted(self):
        self.rng_all(lambda rows: rows[:2] if len(rows) == 3 else rows)
        self.expect(1, 'rng: DIFFERENT')

    def test_window_non_window_label(self):
        def f(rows):
            for r in rows:
                if r['i'] == 1:
                    r['phase'] = 'session'
            return rows
        self.rng_all(f)
        self.expect(1, 'rng: DIFFERENT')

    def test_prefix_label_still_compared(self):
        def f(rows):
            rows[0]['phase'] = 'effect-grid'
            return rows
        self.rng_all(f)
        self.expect(1, 'rng: DIFFERENT')

    def test_native_derived_key_refused(self):
        self.rng_all(lambda rows: [dict(r, derived='lcg-step') for r in rows])
        self.expect(2)

    def test_original_derived_without_light_phase(self):
        p = self.f.orig / 'empty-ready' / 'rng.jsonl'
        rows = [json.loads(x) for x in p.read_text().splitlines()]
        rows[-1]['phase'] = 'restore-ui-glyph'
        p.write_text(base.jl(rows))
        self.f.pin()
        self.expect(2)

    def test_original_light_phase_without_tag(self):
        p = self.f.orig / 'empty-ready' / 'rng.jsonl'
        rows = [json.loads(x) for x in p.read_text().splitlines()]
        del rows[-1]['derived']
        p.write_text(base.jl(rows))
        self.f.pin()
        self.expect(2)

    def test_lifecycle_and_frames_still_compared(self):
        self.f.set_frames([r for r in base.FRAMES if r['t'] != 2])
        self.expect(1, 'INCOMPLETE native coverage')
        self.f.set_frames(base.FRAMES)
        rows = self.f.native_rows('second-title-return')
        rows = [r for r in rows if r['kind'] != 'upload-bytes']
        self.f.rewrite('second-title-return', 'lifecycle.jsonl', rows)
        self.expect(1, 'events: DIFFERENT')

    def test_asserted_rows_unproven(self):
        for cp in CPS:
            rows = self.f.native_rows(cp)
            for r in rows:
                if r['kind'] == 'ui':
                    r['asserted'] = True
            self.f.rewrite(cp, 'lifecycle.jsonl', rows)
        self.expect(4, 'UNPROVEN')

    def test_unpinned_light_original(self):
        p = self.f.orig / 'first-layout' / 'state.json'
        p.write_text(p.read_text() + ' ')
        self.expect(2)

class LcgUnits(unittest.TestCase):
    def test_lcg_rule(self):
        self.assertEqual(ext.lcg(0x41c64e6d)[0], (0x41c64e6d * 0x41c64e6d + 0x3039) & 0xffffffff)
        s, v = ext.lcg(0x80000000)
        self.assertEqual(v, s & 0x7fffffff)

    def test_lcg_steps(self):
        s = 12345
        x = s
        for _ in range(37):
            x, _ = ext.lcg(x)
        self.assertEqual(ext.lcg_steps(s, x), 37)
        with self.assertRaises(ext.Fail):
            ext.lcg_steps(s, s + 1, limit=1000)

    def test_light_traversal_ordinals(self):
        ev = [{'i': 0, 'ev': 'checkpoint'}, {'i': 1, 'ev': 'enter', 'name': 'x'},
              {'i': 2, 'ev': 'enter', 'name': 'restore_input'}, {'i': 3, 'ev': 'exit', 'name': 'restore_input'},
              {'i': 4, 'ev': 'enter', 'name': 'restore_input'}, {'i': 5, 'ev': 'checkpoint'},
              {'i': 6, 'ev': 'top', 'pc': '0x100618'}, {'i': 7, 'ev': 'enter', 'name': 'y'}]
        t = ext.light_t(ev, 0, 5)
        self.assertEqual([t[i] for i in range(8)], [0, 0, 0, 0, 1, 1, 2, 2])

class LightOriginalDerived(unittest.TestCase):
    """SYNTHETIC native-like fixture copied from the pinned LIGHT extract; not native output."""

    @classmethod
    def setUpClass(cls):
        if not (LIGHT / 'reference.json').exists() or not lc.MANIFEST.exists():
            raise unittest.SkipTest('Restore light evidence not present (set OKAGE_MAIN_CHECKOUT)')

    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.nat = Path(self.td.name) / 'native'
        for cp in CPS:
            o, n = LIGHT / cp, self.nat / cp
            n.mkdir(parents=True)
            shutil.copyfile(o / 'events.jsonl', n / 'lifecycle.jsonl')
            for x in ('state.json', 'effects.json'):
                shutil.copyfile(o / x, n / x)
            rows = [json.loads(x) for x in (o / 'rng.jsonl').read_text().splitlines()]
            (n / 'rng.jsonl').write_text(base.jl([{k: v for k, v in dict(r, phase='restore-ui-glyph' if r['phase'] == 'restore-ui' else r['phase']).items()
                                                   if k != 'derived'} for r in rows]))
            shutil.copyfile(o / 'vram-written-values.bin', n / 'vram.bytes')
            shutil.copyfile(o / 'vram-written-mask.bin', n / 'vram.known')
            shutil.copyfile(o / 'vram-invalidated-mask.bin', n / 'vram.invalidated')
        rows = [json.loads(x) for x in (LIGHT / 'manager-frames.jsonl').read_text().splitlines()]
        (self.nat / 'manager-frames.jsonl').write_text(base.jl([{k: r[k] for k in g.FRAME_KEYS} for r in rows]))
        ref = json.loads((LIGHT / 'reference.json').read_text())
        (self.nat / 'route.json').write_text(json.dumps({'route': 'restore-cancel', 'elf_sha256': base.ELF,
                                                         'checkpoints': {cp: ref['checkpoints'][cp]['t'] + 1000 for cp in CPS}}))

    def tearDown(self):
        self.td.cleanup()

    def run_cmp(self):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            code = lc.main([str(LIGHT), str(self.nat)])
        return code, out.getvalue()

    def test_faithful_copy_identical(self):
        code, out = self.run_cmp()
        self.assertEqual(code, 0, out)

    def test_light_draw_corruption(self):
        for cp in ('empty-ready', 'cancel-death', 'resource-retirement', 'wrapper-free', 'second-title-return'):
            p = self.nat / cp / 'rng.jsonl'
            rows = [json.loads(x) for x in p.read_text().splitlines()]
            rows[-1]['state'] ^= 1
            p.write_text(base.jl(rows))
        code, out = self.run_cmp()
        self.assertEqual(code, 1, out)

    def test_card_timing_shift_is_not_identical(self):
        p = self.nat / 'manager-frames.jsonl'
        rows = [json.loads(x) for x in p.read_text().splitlines()]
        rows[1]['update_state_out'] = 510
        p.write_text(base.jl(rows))
        code, out = self.run_cmp()
        self.assertEqual(code, 1, out)

if __name__ == '__main__':
    unittest.main()
