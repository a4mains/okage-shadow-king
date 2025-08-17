#!/usr/bin/env python3
"""Compare a native Session Restore-cancel report with the Restore LIGHT original reference.

usage: restore_light_compare.py LIGHT_ANALYSIS_DIR NATIVE_DIR [--manifest PATH] [--json OUT] [--strict]

LIGHT_ANALYSIS_DIR is written by `tools/restore_original_extract.py --light` from
`okage-startup-oracle --restore-cancel --light-restore` captures (the light Restore reference contract): the
same eight checkpoints, boundary rule, lifecycle vocabulary, state, effect106, VRAM and manager-frames as the full Restore
reference, but between retire-successor-entry and cancel-death no rand(), card, UI placement/layout or probe breakpoint
is armed, so the card thread is not slowed. Draws in that window are DERIVED by stepping the Session LCG between
observed rand states; their rows carry "phase": "restore-ui" and "derived": "lcg-step" and their call sites are NOT
observed.

NATIVE_DIR: exactly the layout tools/restore_compare.py accepts. Everything is compared as there (exit codes,
asserted-row provenance, frame coverage, timing anchors), except draws: a derived original row is compared on
(i, state, value), and the native phase must be one of restore-ui-glyph|restore-ui-icon|restore-ui-select (mapped to
"restore-ui"); every non-derived original row (the startup prefix) is compared on all fields including the label.
Original rows may carry "derived" only as "lcg-step" with phase "restore-ui"; native rows never carry it.
Default --manifest: docs/restore-light-manifest.json. Research tool; never product input.
"""
import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import restore_compare as g  # noqa: E402

MANIFEST = Path(__file__).resolve().parents[1] / 'docs/restore-light-manifest.json'
LIGHT_PHASE = 'restore-ui'
WINDOW_LABELS = ('restore-ui-glyph', 'restore-ui-icon', 'restore-ui-select')
Malformed = g.Malformed

_base_load_rng = g.load_rng
_base_compare_rng = g.compare_rng

def load_rng(path, native=False):
    if native:
        rows, asserted = _base_load_rng(path, native=True)
        return rows, asserted
    rows, derived = [], []
    for n, d in g.jsonl_rows(path):
        if not isinstance(d, dict):
            raise Malformed(f'{path}:{n}: rng row')
        if 'derived' in d or d.get('phase') == LIGHT_PHASE:
            if d.get('derived') != 'lcg-step' or d.get('phase') != LIGHT_PHASE:
                raise Malformed(f'{path}:{n}: a derived row must carry phase "restore-ui" and derived "lcg-step"')
            d = {k: v for k, v in d.items() if k != 'derived'}
            flag = True
        else:
            flag = False
        if set(d) != {'i', 'phase', 'state', 'value'}:
            raise Malformed(f'{path}:{n}: rng row keys')
        if not (g.is_int(d['i']) and g.is_int(d['state']) and g.is_int(d['value'])) \
                or (d['phase'] not in g.RNG_PHASES and d['phase'] != LIGHT_PHASE) \
                or not 0 <= d['state'] <= 0xffffffff or not 0 <= d['value'] <= 0xffffffff or d['i'] != len(rows):
            raise Malformed(f'{path}:{n}: rng row {d!r}')
        rows.append(d)
        derived.append(flag)
    return rows, derived

def compare_rng(orig, nat, nat_asserted, out):
    def view(i):
        o = orig[i]
        if o['phase'] != LIGHT_PHASE:
            return o, nat[i]
        n = dict(nat[i])
        if n['phase'] in WINDOW_LABELS:
            n['phase'] = LIGHT_PHASE
        return o, n
    for i in range(max(len(orig), len(nat))):
        if i >= len(orig) or i >= len(nat) or view(i)[0] != view(i)[1]:
            print(f'    rng: DIFFERENT at draw {i} (of {len(orig)} original / {len(nat)} native): original '
                  f'{orig[i] if i < len(orig) else "<absent>"} | native {nat[i] if i < len(nat) else "<absent>"}', file=out)
            return 1, i, sum(nat_asserted)
    d = sum(1 for o in orig if o['phase'] == LIGHT_PHASE)
    print(f'    rng: identical ({len(orig)} draws; {d} light-window draws compared on i/state/value with native labels '
          f'mapped to "restore-ui"; {sum(nat_asserted)} owner-asserted)', file=out)
    return 0, None, sum(nat_asserted)

def run(orig_dir, nat_dir, out=None, manifest=MANIFEST, strict=False):
    saved = (g.load_rng, g.compare_rng)
    g.load_rng, g.compare_rng = load_rng, compare_rng
    try:
        return g.run(orig_dir, nat_dir, out=out, manifest=manifest, strict=strict)
    finally:
        g.load_rng, g.compare_rng = saved

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('original')
    p.add_argument('native')
    p.add_argument('--manifest', default=str(MANIFEST))
    p.add_argument('--json')
    p.add_argument('--strict', action='store_true', help='zero VRAM coverage gaps required')
    a = p.parse_args(argv)
    try:
        code, result = run(a.original, a.native, manifest=a.manifest, strict=a.strict)
    except Malformed as exc:
        print('MALFORMED: ' + str(exc), file=sys.stderr)
        return 2
    result['reference'] = 'light'
    if a.json:
        with open(a.json, 'x') as f:
            json.dump(result, f, indent=1)
            f.write('\n')
    return code

if __name__ == '__main__':
    sys.exit(main())
