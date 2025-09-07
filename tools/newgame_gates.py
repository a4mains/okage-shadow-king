#!/usr/bin/env python3
"""Newgame completion gates, evaluated mechanically from retained evidence.

usage: newgame_gates.py [--json OUT]     (repository root)

Each gate has a fixed expected value declared below; nothing is interpreted after the fact. Parity gates require the
comparator's IDENTICAL (exit 0) or the control-exactness pass (exit 0). The two NEGATIVE-CONTROL gates require that the
comparator does NOT pass a run whose inputs differ from the reference (exit 3, ANCHORS-DIFFER): a 0 there would be a
comparator defect. Exit 0 iff every gate passes.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path.cwd()
V = 'local/research/newgame-validation-v1'
F = f'{V}/final'
NO = 'local/research/newgame-no-route-v2'

def val(path):
    return (ROOT / path).read_text().strip()

def last_result(path, prefix):
    lines = [l for l in val(path).splitlines() if l.startswith(prefix)]
    return lines[-1] if lines else None

GATES = []

def gate(name, kind, path, expected, read=val):
    try:
        got = read(path)
    except OSError as exc:
        got = f'missing: {exc}'
    GATES.append({'gate': name, 'kind': kind, 'evidence': path, 'expected': expected, 'got': got, 'pass': got == expected})

def main(argv):
    out = argv[argv.index('--json') + 1] if '--json' in argv else None
    # Original parity (fresh real Sessions, pinned binaries, strict).
    for b in ('release', 'sanitize'):
        gate(f'Yes route parity, {b}', 'parity', f'{F}/sessions/{b}-scripted/compare.exit', '0')
        gate(f'No branch parity vs original dense No reference, {b}', 'parity', f'{NO}/{b}-dense/dense-compare.exit', '0')
        gate(f'No branch exactness vs original minimal control, {b}', 'parity', f'{NO}/{b}-control/control-exactness.exit', '0')
        for s in ('scripted', 'no-then-yes', 'fixed'):
            gate(f'{b}-{s}: run exit', 'run', f'{F}/sessions/{b}-{s}/exit.txt', '0')
            gate(f'{b}-{s}: consumed stage reads match the manifest', 'inputs', f'{F}/sessions/{b}-{s}/reads-check.exit', '0')
            gate(f'{b}-{s}: isolated save directory stays empty', 'preservation', f'{F}/sessions/{b}-{s}/saves-after.txt', '')
    gate('later play continues past the stop (2400 frames), stop still IDENTICAL', 'parity',
         f'{F}/sessions/release-continue/compare.exit', '0')
    # Negative controls: inputs differ from the Yes reference, so the comparator must refuse a pass.
    for b in ('release', 'sanitize'):
        gate(f'negative control: raw-frame tape vs Yes reference is inconclusive, {b}', 'negative-control',
             f'{F}/sessions/{b}-fixed/compare.exit', '3')
        gate(f'negative control: control-tape No run vs dense No reference is inconclusive, {b}', 'negative-control',
             f'{NO}/{b}-control/dense-compare.exit', '3')
    # Earlier texture gates on this source.
    for b in ('release', 'sanitize'):
        gate(f'Restore light reference strict, {b}', 'earlier-gate', f'{F}/restore-{b}-scripted/light-compare.exit', '0')
        gate(f'Restore minimal-control exactness, {b}', 'earlier-gate', f'{F}/restore-{b}-scripted/io-timing.exit', '0')
        gate(f'Restore pending-scan Cancel lifetime, {b}', 'earlier-gate', f'{F}/restore-{b}-pending/exit.txt', '0')
        for k in ('rng', 'timeline', 'startup', 'title'):
            gate(f'Startup/RNG/Title gate {k}, {b}', 'earlier-gate', f'{F}/gates-{b}/{k}.txt', '0',
                 read=lambda p: val(p).splitlines()[-1])
    for r in ('restore', 'newgame'):
        gate(f'Retire {r} strict', 'earlier-gate', f'{F}/retire-{r}/compare.exit', '0')
    gate('cold start 1800 old/new: every pair ALIGNED-IDENTICAL', 'earlier-gate', f'{F}/regression-cold/align.txt', 'True',
         read=lambda p: str(all(l == 'result: ALIGNED-IDENTICAL' for l in val(p).splitlines() if l.startswith('result:'))
                            and sum(1 for l in val(p).splitlines() if l.startswith('result:')) == 6))
    # Tests.
    for b in ('release', 'debug', 'sanitize'):
        gate(f'CTest {b} (all)', 'tests', f'{V}/ctest-{b}-all.out', '100% tests passed, 0 tests failed out of 111',
             read=lambda p: last_result(p, '100% tests passed') or last_result(p, 'tests passed'))
    gate('full Python suite after the final tool changes', 'tests', f'{NO}/python-suite.out', 'OK (skipped=95)',
         read=lambda p: last_result(p, 'OK') or last_result(p, 'FAILED'))
    gate('Newgame CTests after the final tool changes (Release)', 'tests', f'{NO}/ctest-release-newgame.out',
         '100% tests passed, 0 tests failed out of 3', read=lambda p: last_result(p, '100% tests passed') or 'failed')
    # Fix first: the counter defect reproduced against the original's value at the same call.
    gate('counter defect reproduced (old 845, new 793; original 792 -> 793)', 'fix-first',
         'local/research/newgame-counter-defect-v2/counter-comparison.txt', 'True',
         read=lambda p: str('"image_counter_after_first_tick": 845' in val(p) and '"image_counter_after_first_tick": 793' in val(p)))
    ok = all(g['pass'] for g in GATES)
    for g in GATES:
        print(f"{'PASS' if g['pass'] else 'FAIL'}  [{g['kind']}] {g['gate']}  (expected {g['expected']!r}, got {g['got']!r})")
    print(f"Newgame gates: {sum(g['pass'] for g in GATES)}/{len(GATES)} pass -> {'ALL PASS' if ok else 'FAILED'}")
    if out:
        Path(out).write_text(json.dumps({'gates': GATES, 'all_pass': ok}, indent=1) + '\n')
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
