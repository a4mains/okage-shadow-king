#!/usr/bin/env python3
"""Compare field-message window RNG draws between an original oracle trace and a native run.

Original: one or more consecutive okage-boot-oracle `--rng` JSON Lines segments (each
segment's frames restart at 0; segments are concatenated in the given order). Calls whose
return address is a placement site are classified: 0x19c754 glyph (19c568, with the text
object), 0x19cadc icon (19ca20 first draw) and 0x19cc80 select (19ca20 second draw).

Native: okage-native `--rng-sites` JSON Lines (per frame: glyph/icon/select/other counts and,
on frames with a message resource pass, the message windows' accounting).

Both streams are anchored at the message construction pass: the first frame whose placement
draws are exactly the two 16-icon constructor passes (original: 32 icon + 32 select calls and no
glyph; native: message.b_constructor true). Phase-8 messages have no constructor pass; pass
--original-anchor/--native-anchor explicitly for them. Frames are never shifted after anchoring.
Per relative frame the comparison is: decoration glyphs, body glyphs, icon draws, select draws.
It reports the first divergence and the frames compared; it is not a media comparison.
"""
import argparse
import json
import sys
from pathlib import Path

GLYPH, ICON, SELECT = 0x19c754, 0x19cadc, 0x19cc80

def original_frames(paths):
    frames, offset = [], 0
    for path in paths:
        rows = [json.loads(line) for line in Path(path).read_text().splitlines()]
        if not rows or rows[0].get('schema') != 'okage-shared-rng-entry':
            raise ValueError(f'{path}: not an oracle shared-RNG trace')
        by = {}
        last = -1
        for row in rows[1:]:
            frame = row['frame']
            if frame < last:
                raise ValueError(f'{path}: frames out of order at sequence {row["sequence"]}')
            last = frame
            entry = by.setdefault(frame, {'objects': [], 'icon': 0, 'select': 0, 'other': 0})
            if row['ra'] == GLYPH:
                entry['objects'].append(row['glyph_text_object'])
            elif row['ra'] == ICON:
                entry['icon'] += 1
            elif row['ra'] == SELECT:
                entry['select'] += 1
            else:
                entry['other'] += 1
        count = max(by) + 1 if by else 0
        for frame in range(count):
            frames.append(by.get(frame, {'objects': [], 'icon': 0, 'select': 0, 'other': 0}))
        offset += count
    return frames

def native_frames(path):
    rows = [json.loads(line) for line in Path(path).read_text().splitlines()]
    if not rows or rows[0].get('schema') != 'okage-native-rng-sites-v1':
        raise ValueError(f'{path}: not a native rng-sites trace')
    out, previous = {}, None
    for row in rows[1:]:
        if previous is not None and row['frame'] != previous + 1:
            raise ValueError(f'{path}: native frames are not contiguous at {row["frame"]}')
        previous = row['frame']
        out[row['frame']] = row
    return out

def original_anchor(frames):
    for i, f in enumerate(frames):
        if f['icon'] == 32 and f['select'] == 32 and not f['objects']:
            return i
    raise ValueError('original trace has no two-window constructor pass')

def native_anchor(frames, message_id):
    for frame in sorted(frames):
        m = frames[frame].get('message')
        if m and m.get('b_constructor') and (message_id is None or m['id'] == message_id):
            return frame
    raise ValueError('native trace has no message constructor pass')

def classify_original(frame, decoration, body):
    glyph_a = sum(1 for o in frame['objects'] if o == decoration)
    glyph_b = sum(1 for o in frame['objects'] if o == body)
    if glyph_a + glyph_b != len(frame['objects']):
        raise ValueError('original glyph calls from a third text object')
    return {'glyph_a': glyph_a, 'glyph_b': glyph_b, 'icon': frame['icon'], 'select': frame['select']}

def classify_native(row):
    m = row.get('message') or {}
    glyph_b = m.get('b_glyphs', 0)
    return {'glyph_a': row['glyph'] - glyph_b, 'glyph_b': glyph_b, 'icon': row['icon'], 'select': row['select']}

def compare(original_paths, native_path, length, message_id=None, oanchor=None, nanchor=None,
            decoration=None, body=None):
    ofr = original_frames(original_paths)
    nfr = native_frames(native_path)
    oa = original_anchor(ofr) if oanchor is None else oanchor
    na = native_anchor(nfr, message_id) if nanchor is None else nanchor
    objects = [o for f in ofr[oa:oa + length] for o in f['objects']]
    seen = []
    for o in objects:
        if o not in seen:
            seen.append(o)
    if decoration is None or body is None:
        if len(seen) != 2:
            raise ValueError(f'cannot infer the two window objects from original glyph calls: {seen}')
        # The decoration window lays out first in registry order on the first mode-2 pass.
        decoration, body = seen
    result = {'original_anchor': oa, 'native_anchor': na, 'decoration_object': decoration,
              'body_object': body, 'requested_frames': length, 'compared': 0,
              'first_divergence': None, 'missing': None,
              'scope': 'placement RNG draw counts per window and site; not image/audio equivalence'}
    for rel in range(length):
        if oa + rel >= len(ofr) or (na + rel) not in nfr:
            result['missing'] = {'relative_frame': rel, 'original_available': oa + rel < len(ofr),
                                 'native_available': (na + rel) in nfr}
            break
        o = classify_original(ofr[oa + rel], decoration, body)
        n = classify_native(nfr[na + rel])
        result['compared'] += 1
        if o != n and result['first_divergence'] is None:
            result['first_divergence'] = {'relative_frame': rel, 'original_frame': oa + rel,
                                          'native_frame': na + rel, 'original': o, 'native': n}
    result['verdict'] = ('IDENTICAL' if result['first_divergence'] is None and result['missing'] is None
                         else 'DIFFERENT' if result['first_divergence'] else 'INCOMPLETE')
    return result

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--original', nargs='+', required=True, type=Path)
    p.add_argument('--native', required=True, type=Path)
    p.add_argument('--frames', required=True, type=int)
    p.add_argument('--message-id', type=int)
    p.add_argument('--original-anchor', type=int)
    p.add_argument('--native-anchor', type=int)
    a = p.parse_args(argv)
    try:
        r = compare(a.original, a.native, a.frames, a.message_id, a.original_anchor, a.native_anchor)
    except (OSError, ValueError, KeyError) as e:
        print(json.dumps({'verdict': 'MALFORMED', 'error': str(e)}))
        return 2
    print(json.dumps(r, indent=1))
    return 0 if r['verdict'] == 'IDENTICAL' else 1

if __name__ == '__main__':
    sys.exit(main())
