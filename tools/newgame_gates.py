#!/usr/bin/env python3
"""Newgame completion gates, evaluated mechanically from retained evidence.

usage: newgame_gates.py [--json OUT]     (repository root)

Each gate has a fixed expected value declared below; every gate is a pass condition (IDENTICAL/exact/exit 0/all tests
passed). Separately, and NOT counted as Newgame gates, "comparator behaviour" checks confirm that the comparator refuses to
pass runs whose inputs differ from the reference (exit 3, ANCHORS-DIFFER). Exit 0 iff every gate passes and every
comparator-behaviour check holds.
"""
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path.cwd()
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
V = 'local/research/newgame-validation-v1'
F = f'{V}/final'
NO = 'local/research/newgame-no-route-v2'

def val(path):
    return (ROOT / path).read_text().strip()

def last_result(path, prefix):
    lines = [l for l in val(path).splitlines() if l.startswith(prefix)]
    return lines[-1] if lines else None

GATES, BEHAVIOUR, HYGIENE = [], [], []
PINNED_COUNTS = {'local/research/newgame-report-binding-v1/native-reports.sha256': 1804,
                 'local/research/newgame-report-binding-v1/postfix-reports.sha256': 984,
                 'local/research/newgame-review-v1.sha256': 4240, 'local/research/newgame-gate-audit-v1.sha256': 24}
YES_REF = 'local/research/newgame-original-v1/analysis/reference.json'
NO_REF = 'local/research/newgame-no-original-v1/analysis/reference.json'
SUITES = 'local/research/newgame-final-suites-v2'
POST = 'local/research/newgame-postfix-sessions-v1'

def jload(p):
    return json.loads(val(p))

def sha_check(listing, base='.'):
    """'N/N' when every line of a shasum listing verifies (paths relative to base)."""
    import hashlib
    ok = n = 0
    for line in val(listing).splitlines():
        h, path = line.split(None, 1)
        n += 1
        try:
            ok += hashlib.sha256((ROOT / base / path.strip()).read_bytes()).hexdigest() == h
        except OSError:
            pass
    return f'{ok}/{n}'

def aligned_old_new(rel, tool):
    """Old/new pairs with equal anchors and the streams that differ in them (from the retained pair JSONs)."""
    out = {}
    for j in sorted((ROOT / rel).glob('*.json')):
        a, b = j.stem.split('-', 1)[0], j.stem.split('-', 1)[1] if '-' in j.stem else ''
        r = json.loads(j.read_text())
        names = j.stem
        if not r.get('anchors_equal') or not (('old' in names) and ('new' in names)):
            continue
        diff = {k: v.get('differing_words') for k, v in r['streams'].items()
                if v.get('first_difference_frame', v.get('first_difference')) is not None}
        out[names] = {'differs': diff, 'audio': r['audio']['first_difference_frame'], 'png': r['final_png_identical']}
    return out

def gate(name, kind, path, expected, read=val, into=None):
    try:
        got = read(path)
    except Exception as exc:
        got = f'unreadable: {type(exc).__name__}: {exc}'
    (GATES if into is None else into).append({'gate': name, 'kind': kind, 'evidence': path, 'expected': expected,
                                              'got': got, 'pass': got == expected})

def _chapter_word5_only(rel):
    import struct
    pairs = aligned_old_new(rel, 'newgame')
    if not pairs:
        return False
    for names in pairs:
        a, b = names.split('-', 1)[0] + '-' + names.split('-')[1], '-'.join(names.split('-')[2:])
        ra = (ROOT / rel / a / 'f.png.chapter-state.bin').read_bytes()
        rb = (ROOT / rel / b / 'f.png.chapter-state.bin').read_bytes()
        A = {w[0]: w for w in struct.iter_unpack('<6I', ra)}
        B = {w[0]: w for w in struct.iter_unpack('<6I', rb)}
        if set(A) != set(B) or any(A[f][i] != B[f][i] for f in A for i in range(6) if i != 5):
            return False
    return True

def main(argv):
    out = argv[argv.index('--json') + 1] if '--json' in argv else None
    # Original parity (fresh real Sessions, pinned binaries, strict).
    for b in ('release', 'sanitize'):
        gate(f'Yes route parity, {b}', 'parity', f'{F}/sessions/{b}-scripted/compare.exit', '0')
        gate(f'No branch parity vs original dense No reference, {b}', 'parity', f'{NO}/{b}-dense/dense-compare.exit', '0')
        gate(f'No branch exactness vs original minimal control, {b}', 'parity', f'{NO}/{b}-control/control-exactness.exit', '0')
        gate(f'Yes route parity with the hardened comparator, {b}', 'parity',
             f'{V}/recheck-hardened/{b}-scripted.json', 'IDENTICAL', read=lambda p: jload(p)['result'])
        gate(f'{b}-scripted: consumed stage reads match the manifest', 'inputs', f'{F}/sessions/{b}-scripted/reads-check.exit', '0')
        gate(f'{b}-scripted: isolated save directory stays empty', 'preservation', f'{F}/sessions/{b}-scripted/saves-after.txt', '')
        for t in ('dense', 'control'):
            gate(f'No route ({t} tape), {b}: consumed reads and empty saves', 'inputs', f'{NO}/{b}-{t}/reads-check.exit', '0')
        for s in ('no-then-yes', 'fixed'):
            gate(f'{b}-{s}: run exit', 'run', f'{F}/sessions/{b}-{s}/exit.txt', '0', into=HYGIENE)
            gate(f'{b}-{s}: consumed stage reads match the manifest', 'inputs', f'{F}/sessions/{b}-{s}/reads-check.exit', '0', into=HYGIENE)
            gate(f'{b}-{s}: isolated save directory stays empty', 'preservation', f'{F}/sessions/{b}-{s}/saves-after.txt', '', into=HYGIENE)
    gate('later play continues past the stop (2400 frames), stop still IDENTICAL', 'parity',
         f'{F}/sessions/release-continue/compare.exit', '0')
    gate('continuation run: consumed reads match, saves stay empty', 'inputs', f'{F}/sessions/release-continue/reads-check.exit', '0')
    gate('continuation run: isolated save directory stays empty', 'preservation', f'{F}/sessions/release-continue/saves-after.txt', '')
    # Original reference health.
    gate('Yes reference: no problems, Retire-neutral, control agrees', 'reference', YES_REF, 'True',
         read=lambda p: str(jload(p)['problems'] == [] and jload(p)['retire_neutrality']['failures'] == []
                            and jload(p)['control_minimal']['failures'] == []))
    gate('Yes reference: every RNG draw hooked (chain-checked), 11,848 draws', 'reference', YES_REF, 'True',
         read=lambda p: str(jload(p)['rng']['provenance'].startswith('every draw is a hooked') and jload(p)['rng']['total_draws'] == 11848))
    import newgame_compare as cmpmod
    gate('No reference: deterministic and Retire-neutral; only problems are control press timing (comparator rule)', 'reference',
         NO_REF, 'True', read=lambda p: str(cmpmod.control_timing_only(jload(p))))
    gate('No-variant oracle leaves the default mode byte-identical', 'reference',
         'local/research/newgame-no-original-v1/neutrality-vs-final-reference.txt', 'True',
         read=lambda p: str(all(l.endswith('identical') for l in val(p).splitlines()) and len(val(p).splitlines()) == 6))
    gate('glyph-cache writer/lookup absent on this route (original)', 'route-fact', YES_REF, '0',
         read=lambda p: str(sum(v for k, v in jload(p)['glyph'].items() if k != 'verdict')))
    gate('CHP_01.XIM consumed natively = the original read (sha256 94ef3841...)', 'route-fact',
         f'{F}/sessions/release-scripted/reads.jsonl', 'True',
         read=lambda p: str(any('CHP_01.XIM' in l and '94ef3841f0b5f1c619b9cb92bc332bf443cdd0b776537f307f320a3d26b16b97' in l
                                for l in val(p).splitlines())
                            and any(r['file'] == 'chp_01.xim' and r['sha256'] == '94ef3841f0b5f1c619b9cb92bc332bf443cdd0b776537f307f320a3d26b16b97'
                                    for r in jload(YES_REF)['disc_reads_after_successor'])))
    # Comparator behaviour (not Newgame gates): runs whose inputs differ from a reference must not pass.
    for b in ('release', 'sanitize'):
        gate(f'raw-frame tape vs Yes reference is not passed, {b}', 'comparator-behaviour',
             f'{F}/sessions/{b}-fixed/compare.exit', '3', into=BEHAVIOUR)
        gate(f'control-tape No run vs dense No reference is not passed, {b}', 'comparator-behaviour',
             f'{NO}/{b}-control/dense-compare.exit', '3', into=BEHAVIOUR)
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
    def newgame_ok(p):
        al = aligned_old_new(p, 'newgame')
        return str(bool(al) and all(set(v['differs']) <= {'chapter-state.bin', 'game-mode.bin'}
                                    and ('game-mode.bin' not in v['differs'] or v['differs']['game-mode.bin'] == [5])
                                    and v['audio'] is None and v['png']
                                    for v in al.values()))
    gate('old/new New Game raw-frame tape: aligned pairs differ only in the counter and current[2]', 'regression',
         f'{F}/regression-newgame', 'True', read=newgame_ok)
    gate('old/new aligned-pair counts (New Game, Restore) as retained', 'regression', f'{F}', '[2, 5]',
         read=lambda p: str([len(aligned_old_new(f'{p}/regression-newgame', 'newgame')), len(aligned_old_new(f'{p}/regression-restore-cancel', 'retire'))]))
    gate('old/new New Game: chapter-state difference is word 5 (image counter) only', 'regression',
         f'{F}/regression-newgame', 'True',
         read=lambda p: str(_chapter_word5_only(p)))
    gate('old/new Restore tape: aligned pairs differ only in game-mode.bin; audio and image equal', 'regression',
         f'{F}/regression-restore-cancel', 'True',
         read=lambda p: str(bool(aligned_old_new(p, 'retire')) and all(set(v['differs']) <= {'game-mode.bin'} and v['audio'] is None and v['png']
                                                                for v in aligned_old_new(p, 'retire').values())))
    for b in ('release', 'sanitize'):
        for k, what in (('yes', 'Yes route vs original (strict)'), ('no-dense', 'No branch vs original dense No (strict)'),
                        ('no-control', 'No branch exact vs original minimal control')):
            gate(f'post-fix {what}, {b}', 'parity', f'{POST}/{b}-{k}/compare.exit', '0')
            gate(f'post-fix {k} {b}: consumed reads match the manifest', 'inputs', f'{POST}/{b}-{k}/reads-check.exit', '0')
    # Tests: complete suites rerun after the final source/tool/test change.
    for b in ('release', 'debug', 'sanitize'):
        gate(f'CTest {b} (all), after the final change', 'tests', f'{SUITES}/ctest-{b}-all.out', 'True',
             read=lambda p: str(any(l.startswith('100% tests passed, 0 tests failed out of') for l in val(p).splitlines())))
    gate('full Python suite after the final change (all Newgame comparator tests ran: >= 1,182 tests)', 'tests', f'{SUITES}/python-suite.out', 'True',
         read=lambda p: str(any(l.startswith('OK') for l in val(p).splitlines()) and not any(l.startswith('FAILED') for l in val(p).splitlines())
                            and int(next(l for l in val(p).splitlines() if l.startswith('Ran ')).split()[1]) >= 1182))
    # Fix first: the counter defect, against the ORIGINAL's values.
    gate('original: counter constant (792) over every name-entry call, 290 steps all in the chapter', 'fix-first', YES_REF, 'True',
         read=lambda p: str(jload(p)['image_counter']['name_input_values'] == [792] and jload(p)['image_counter']['steps_after_successor'] == 290
                            and jload(p)['image_counter']['step_traversals'] == [52, 341]))
    gate('original: chapter call 1 sees 792, call 2 sees 793', 'fix-first',
         'local/research/newgame-original-v1/analysis/chapter-frames.jsonl', '[792, 793]',
         read=lambda p: str([json.loads(l)['image_counter_in'] for l in val(p).splitlines()[:2]]))
    gate('defect reproduced: old binary 845, fixed binary 793 after the first chapter call (same inputs, same S)', 'fix-first',
         'local/research/newgame-counter-defect-v2/counter-comparison.txt', 'True',
         read=lambda p: str((lambda r: r['old']['first']['image_counter_after_first_tick'] == 845
                                       and r['new']['first']['image_counter_after_first_tick'] == 793
                                       and r['old']['first']['frame'] == r['new']['first']['frame'])(
                               json.loads(val(p)[:val(p).index('\noriginal')]))))
    gate('fixed native: counter constant over name entry, +290 over the chapter (scripted session)', 'fix-first',
         f'{F}/sessions/release-scripted/report', 'True',
         read=lambda p: str(len({jload(f'{p}/{c}/session.json')['image_counter'] for c in ('name-constructor-return', 'name-death',
                                                                                           'command-published', 'chapter-constructor-return')}) == 1
                            and jload(f'{p}/field-constructor-entry/session.json')['image_counter']
                            - jload(f'{p}/chapter-constructor-return/session.json')['image_counter'] == 290))
    # Restore metadata reconciliation and v3 preservation.
    gate('Restore manifest: corrected scope (LCG-derived 4,364) and v3 driver command, errata kept', 'restore-metadata', 'docs/restore-ownership-validation.json', 'True',
         read=lambda p: str('4,364' in jload(p)['scope'] and 'LCG' in jload(p)['scope']
                            and jload(p)['commands']['driver'].startswith('RESTORE_VALIDATION=local/research/restore-ownership-validation-v3') and 'errata' in jload(p)))
    gate('Restore v3 evidence unchanged (every hash in docs/restore-ownership-validation.json)', 'restore-metadata', 'docs/restore-ownership-validation.json', 'True',
         read=lambda p: str(all(__import__('hashlib').sha256((ROOT / e['path']).read_bytes()).hexdigest() == e['sha256']
                                for e in jload(p)['evidence'])))
    # Hash binding and retained review/audit.
    for listing, base in (('local/research/newgame-report-binding-v1/native-reports.sha256', '.'),
                          ('local/research/newgame-report-binding-v1/postfix-reports.sha256', '.'),
                          ('local/research/newgame-review-v1.sha256', 'local/research/newgame-review-v1'),
                          ('local/research/newgame-gate-audit-v1.sha256', 'local/research/newgame-gate-audit-v1')):
        n = PINNED_COUNTS[listing]
        gate(f'hash binding verifies: {listing}', 'binding', listing, f'{n}/{n}', read=lambda p, b=base: sha_check(p, b))
    for m in ('docs/newgame-validation.json',):
        gate(f'{m}: every evidence hash verifies', 'binding', m, 'True',
             read=lambda p: str(len(jload(p)['evidence']) >= 377 and all(
                 __import__('hashlib').sha256((ROOT / e['path']).read_bytes()).hexdigest() == e['sha256'] for e in jload(p)['evidence'])))
    for m, ev in (('docs/newgame-original-manifest.json', 'local/research/newgame-original-v1'),
                  ('docs/newgame-no-original-manifest.json', 'local/research/newgame-no-original-v1')):
        gate(f'{m}: every pinned output verifies', 'binding', m, 'True',
             read=lambda p, e=ev: str(len(jload(p)['outputs']) > 0 and all(
                 __import__('hashlib').sha256((ROOT / e / k).read_bytes()).hexdigest() == v['sha256'] for k, v in jload(p)['outputs'].items())))
    # Preservation (current time).
    gate('preview PID 63851 alive and untouched', 'preservation', '-', 'True',
         read=lambda _: str(__import__('subprocess').run(['ps', '-p', '63851'], capture_output=True).returncode == 0))
    gate('no git remote (nothing pushed)', 'preservation', '-', '',
         read=lambda _: __import__('subprocess').run(['git', 'remote'], capture_output=True, text=True).stdout.strip())
    ok = all(g['pass'] for g in GATES)
    bok = all(g['pass'] for g in BEHAVIOUR) and all(g['pass'] for g in HYGIENE)
    for g in HYGIENE:
        print(f"{'HOLDS' if g['pass'] else 'BROKEN'}  [hygiene, not a gate] {g['gate']}")
    for g in GATES:
        print(f"{'PASS' if g['pass'] else 'FAIL'}  [{g['kind']}] {g['gate']}  (expected {g['expected']!r}, got {g['got']!r})")
    print(f"Newgame gates: {sum(g['pass'] for g in GATES)}/{len(GATES)} pass -> {'ALL PASS' if ok else 'FAILED'}")
    for g in BEHAVIOUR:
        print(f"{'HOLDS' if g['pass'] else 'BROKEN'}  [comparator-behaviour, not a gate] {g['gate']}  (expected {g['expected']!r}, got {g['got']!r})")
    print(f"comparator behaviour: {sum(g['pass'] for g in BEHAVIOUR)}/{len(BEHAVIOUR)} hold")
    ok = ok and bok
    if out:
        Path(out).write_text(json.dumps({'gates': GATES, 'comparator_behaviour': BEHAVIOUR, 'hygiene': HYGIENE,
                                         'all_pass': ok}, indent=1) + '\n')
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
