import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import compare_message_rng as c  # noqa: E402

A, B = 0x95e270, 0x9d7a90
# Relative frames: constructor pass, A glyphs + A/B sentinels, B-only sentinels, nothing.
PATTERN = [
    {'a': 0, 'b': 0, 'icons': 32},
    {'a': 15, 'b': 0, 'icons': 23},
    {'a': 0, 'b': 0, 'icons': 16},
    {'a': 0, 'b': 0, 'icons': 0},
    {'a': 16, 'b': 1, 'icons': 23},
]

def original_rows(pattern, lead=2, third=False):
    rows, seq = [{'schema': 'okage-shared-rng-entry'}], 0
    def call(frame, ra, obj=None):
        nonlocal seq
        rows.append({'sequence': seq, 'frame': frame, 'ra': ra, 'glyph_text_object': obj}); seq += 1
    for f in range(lead):
        call(f, 0x137284)
    for i, p in enumerate(pattern):
        f = lead + i
        for _ in range(p['a']):
            call(f, c.GLYPH, A)
        for _ in range(p['b']):
            call(f, c.GLYPH, B)
        if third:
            call(f, c.GLYPH, 0x123)
        for _ in range(p['icons']):
            call(f, c.ICON); call(f, c.SELECT)
        call(f, 0x137284)
    return rows

def native_rows(pattern, start=100, message_id=99099):
    rows = [{'schema': 'okage-native-rng-sites-v1'}]
    for i, p in enumerate(pattern):
        row = {'frame': start + i, 'other': 1, 'glyph': p['a'] + p['b'], 'icon': p['icons'], 'select': p['icons']}
        row['message'] = {'id': message_id, 'b_glyphs': p['b'], 'b_constructor': i == 0}
        rows.append(row)
    return rows

class CompareMessageRngTests(unittest.TestCase):
    def write(self, name, rows):
        path = Path(self.dir.name) / name
        path.write_text(''.join(json.dumps(r) + '\n' for r in rows))
        return path

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def run_compare(self, original, native, frames=len(PATTERN)):
        return c.compare([self.write('o.jsonl', original)], self.write('n.jsonl', native), frames, 99099)

    def test_identical_pattern_is_anchored_at_the_constructor_pass(self):
        r = self.run_compare(original_rows(PATTERN), native_rows(PATTERN))
        self.assertEqual(r['verdict'], 'IDENTICAL')
        self.assertEqual((r['original_anchor'], r['native_anchor'], r['compared']), (2, 100, 5))
        self.assertEqual((r['decoration_object'], r['body_object']), (A, B))

    def test_missing_glyph_reports_first_divergence(self):
        changed = [dict(p) for p in PATTERN]
        changed[4]['b'] = 0
        r = self.run_compare(original_rows(PATTERN), native_rows(changed))
        self.assertEqual(r['verdict'], 'DIFFERENT')
        self.assertEqual(r['first_divergence']['relative_frame'], 4)

    def test_extra_sentinels_and_shifted_frames_differ(self):
        changed = [dict(p) for p in PATTERN]
        changed[2]['icons'] = 32
        self.assertEqual(self.run_compare(original_rows(PATTERN), native_rows(changed))['verdict'], 'DIFFERENT')
        shifted = PATTERN[:3] + [PATTERN[4], PATTERN[3]]
        self.assertEqual(self.run_compare(original_rows(PATTERN), native_rows(shifted))['verdict'], 'DIFFERENT')

    def test_truncated_native_is_incomplete(self):
        r = self.run_compare(original_rows(PATTERN), native_rows(PATTERN[:3]))
        self.assertEqual(r['verdict'], 'INCOMPLETE')
        self.assertEqual(r['missing']['relative_frame'], 3)

    def test_third_text_object_and_missing_anchor_are_malformed(self):
        with self.assertRaises(ValueError):
            self.run_compare(original_rows(PATTERN, third=True), native_rows(PATTERN))
        with self.assertRaises(ValueError):
            self.run_compare(original_rows(PATTERN[1:]), native_rows(PATTERN))

    def test_ambiguous_original_anchor_and_undeclared_segments_are_rejected(self):
        twice = original_rows(PATTERN) + original_rows(PATTERN, lead=0)[1:]
        for i, row in enumerate(twice[1:]):
            row['sequence'] = i
        later = len(PATTERN) + 2
        for row in twice[len(original_rows(PATTERN)):]:
            row['frame'] += later
        with self.assertRaises(ValueError):
            self.run_compare(twice, native_rows(PATTERN))
        o = self.write('o1.jsonl', original_rows(PATTERN))
        with self.assertRaises(ValueError):
            c.compare([o, o], self.write('n.jsonl', native_rows(PATTERN)), 3, 99099)
        r = c.compare([o, o], self.write('n2.jsonl', native_rows(PATTERN)), 5, 99099,
                      oanchor=2, segment_vblanks=[len(PATTERN) + 4, len(PATTERN) + 2])
        self.assertEqual(r['verdict'], 'IDENTICAL')

    def test_noncontiguous_native_frames_are_rejected(self):
        rows = native_rows(PATTERN)
        rows[3]['frame'] += 1
        with self.assertRaises(ValueError):
            self.run_compare(original_rows(PATTERN), rows)

if __name__ == '__main__':
    unittest.main()
