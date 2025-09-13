#!/usr/bin/env python3
"""Write docs/newgame-validation.json: hashes of Newgame sources, pinned binaries, inputs and retained evidence.

usage: NEWGAME_VALIDATION=local/research/newgame-validation-vN python3 tools/newgame_validation_manifest.py SUBDIR OUT.json
       (repository root; after `NEWGAME_VALIDATION=... tools/newgame_validation_run.sh SUBDIR`)
Results are read from retained output files, not typed in. Current-time audits (re-hashing what exists now) are
labelled separately from run-time evidence recorded when the runs happened.
"""
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path.cwd()
SCOPE = ("Newgame: the FIRST cold title's New Game branch on the persistent Session texture owner and RNG, from entry of "
         "124548 (mode 9) through name-entry construction (wrapper-owned bm_if atlas), an actual name edit and Yes, "
         "manager death/unlink, the published command {14,1,0} and request {2,72,0}, resource/wrapper retirement and "
         "frees, Chapter 1 (CHP_01 image list, counter-240 fade request, actual fade completion, command clear and "
         "request), chapter retirement and frees, to the ENTRY of the second 133830 (command 0), before its body. "
         "Fixes first: the name-entry image-counter defect (reproduced, corrected), the mode-parent +0x4c write of "
         "1246b0, and the Restore metadata. Provenance: every original RNG row is a hooked observation; every native "
         "lifecycle row is observed (none asserted). Not original image/audio parity, ordinary field/XTA/battle "
         "loading, saved-field Restore, shared-GS sampling, performance, tick37297, the full texture port or the game.")

def sha(path):
    h = hashlib.sha256()
    with open(ROOT / path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def entry(path):
    return {'path': path, 'sha256': sha(path), 'bytes': (ROOT / path).stat().st_size}

def text(path):
    return (ROOT / path).read_text().strip()

def results(path, key='result: '):
    out, cur = {}, None
    for line in text(path).splitlines():
        if line.startswith('== '):
            cur = line[3:]
        elif line.startswith(key):
            out[cur] = line[len(key):]
    return out

def main(sub, out):
    V = os.environ.get('NEWGAME_VALIDATION')
    if not V:
        raise SystemExit('set NEWGAME_VALIDATION')
    F = f'{V}/{sub}'
    sources = [
        'include/okage/startup_textures.hpp', 'src/startup_textures.cpp', 'src/native_runtime.mm',
        'src/name_entry_session.hpp', 'src/name_entry_session.mm', 'src/chapter_session.hpp', 'src/chapter_session.mm',
        'src/game_mode.cpp', 'include/okage/game_mode.hpp', 'src/name_entry.cpp', 'src/name_entry_layout.cpp',
        'src/chapter.cpp', 'src/ui_window_glyph.cpp', 'include/okage/rng_site.hpp', 'src/texture_handles.cpp', 'CMakeLists.txt',
        'tools/play-oracle/startup.cpp', 'tools/play-oracle/CMakeLists.txt', 'tools/newgame_capture.sh',
        'tools/newgame_original_extract.py', 'tools/newgame_compare.py', 'tools/newgame_make_manifest.py',
        'tools/newgame_route_compare.py', 'tools/newgame_route_regression.sh', 'tools/newgame_validation_run.sh',
        'tools/newgame_validation_manifest.py', 'tools/restore_validation_manifest.py', 'tools/retire_input_provenance.py',
        'tests/startup_textures_tests.cpp', 'tests/game_mode_tests.cpp', 'tests/name_entry_tests.cpp',
        'tests/test_newgame_routes.py', 'tests/test_newgame_compare.py', 'tests/test_newgame_route_compare.py',
        'tests/data/newgame-routes/newgame-chapter/name-pad.txt', 'tests/data/newgame-routes/no-then-yes/name-pad.txt',
        'tests/data/newgame-routes/no-then-yes-control/name-pad.txt', 'tools/newgame_control_exactness.py',
        'tests/data/newgame-routes/newgame-fixed/buttons.txt', 'tests/data/newgame-routes/newgame-fixed/directions.txt',
        'tests/data/retire-routes/newgame-scripted/title-pad.txt', 'docs/newgame-original-manifest.json',
        'docs/restore-ownership-validation.json',
    ]
    evidence = [f'{V}/bin/binaries.sha256', f'{V}/bin/source-head.txt', f'{F}/binaries-check.txt', f'{F}/commands.txt',
                f'{F}/source-head.txt', f'{F}/driver-command.txt', f'{F}/stage-audit.txt']
    for n in ('ctest-release-all.out', 'ctest-debug-all.out', 'ctest-sanitize-all.out', 'python-suite.out',
              'python-suite-final.out'):
        if (ROOT / V / n).exists():
            evidence.append(f'{V}/{n}')
    if (ROOT / V / 'recheck-hardened').exists():
        evidence += [f'{V}/recheck-hardened/{p.name}' for p in sorted((ROOT / V / 'recheck-hardened').iterdir())]
    for p in ('local/research/newgame-counter-defect-v2/counter-comparison.txt', 'local/research/newgame-counter-defect-v2/binaries.sha256',
              'local/research/newgame-counter-defect-v2/input/sha256.txt'):
        evidence.append(p)
    sessions = {}
    for d in sorted((ROOT / F / 'sessions').iterdir()):
        rel = f'{F}/sessions/{d.name}'
        names = ['command.txt', 'stdout.txt', 'reads.jsonl', 'reads-check.json', 'saves-before.txt', 'saves-after.txt',
                 'compare.txt', 'compare.json', 'outputs.sha256', 'report/route.json', 'report/name-frames.jsonl',
                 'report/chapter-frames.jsonl', 'report/session-draw-frames.jsonl',
                 'report/field-constructor-entry/state.json', 'report/field-constructor-entry/lifecycle.jsonl',
                 'report/field-constructor-entry/rng.jsonl', 'report/field-constructor-entry/session.json']
        evidence += [f'{rel}/{n}' for n in names if (ROOT / rel / n).exists()]
        c = json.loads(text(f'{rel}/compare.json')) if (ROOT / rel / 'compare.json').exists() else {}
        sessions[d.name] = {
            'exit': int(text(f'{rel}/exit.txt')), 'input_check_exit': int(text(f'{rel}/reads-check.exit')),
            'saves_empty_before_after': [text(f'{rel}/saves-before.txt'), text(f'{rel}/saves-after.txt')],
            'route': json.loads(text(f'{rel}/report/route.json')),
            'original_compare_exit': int(text(f'{rel}/compare.exit')), 'result': c.get('result'),
            'checkpoints_identical': sum(1 for v in c.get('checkpoints', {}).values() if v.get('identical')),
            'asserted': c.get('asserted'), 'coverage_gaps': c.get('coverage_gaps'),
            'anchors_differ': c.get('anchors_differ'),
            'name_frames_first_difference': c.get('name_frames_first_difference'),
            'chapter_frames_first_difference': c.get('chapter_frames_first_difference')}
    gates = {}
    for g in ('release', 'sanitize'):
        rel = f'{F}/gates-{g}'
        gates[g] = {k: text(f'{rel}/{k}.txt').splitlines()[-2:] for k in ('rng', 'timeline', 'startup', 'title')}
        evidence += [f'{rel}/{k}.txt' for k in ('rng', 'timeline', 'startup', 'title')] + [f'{rel}/stdout.txt']
    restore = {}
    for tag in ('release', 'sanitize'):
        rel = f'{F}/restore-{tag}-scripted'
        restore[tag] = {'exit': int(text(f'{rel}/exit.txt')), 'light_reference_exit': int(text(f'{rel}/light-compare.exit')),
                    'light_reference_result': json.loads(text(f'{rel}/light-compare.json')).get('result'),
                    'full_reference_exit': int(text(f'{rel}/compare.exit')),
                    'minimal_control_exactness_exit': int(text(f'{rel}/io-timing.exit')),
                    'input_check_exit': int(text(f'{rel}/reads-check.exit'))}
        evidence += [f'{rel}/{n}' for n in ('light-compare.txt', 'light-compare.json', 'compare.txt', 'io-timing.txt',
                                             'stdout.txt', 'reads-check.json')]
        relp = f'{F}/restore-{tag}-pending'
        restore[tag]['pending_cancel_exit'] = int(text(f'{relp}/exit.txt'))
        evidence += [f'{relp}/stdout.txt']
    retire = {}
    for route in ('restore', 'newgame'):
        rel = f'{F}/retire-{route}'
        retire[route] = {'exit': int(text(f'{rel}/exit.txt')), 'compare_exit': int(text(f'{rel}/compare.exit'))}
        evidence += [f'{rel}/compare.txt', f'{rel}/stdout.txt']
    regressions = {}
    for name, rel in (('newgame-fixed', f'{F}/regression-newgame'), ('restore-cancel', f'{F}/regression-restore-cancel')):
        regressions[name] = results(f'{rel}/compare.txt')
        evidence += [f'{rel}/compare.txt', f'{rel}/binaries.sha256', f'{rel}/stage-manifest.sha256', f'{rel}/input/sha256.txt']
        for run in sorted(p.name for p in (ROOT / rel).iterdir() if re.fullmatch(r'(old|new)-[a-d]', p.name)):
            evidence += [f'{rel}/{run}/command.txt', f'{rel}/{run}/outputs.sha256', f'{rel}/{run}/stdout.txt']
    cold = f'{F}/regression-cold'
    regressions['cold-1800'] = results(f'{cold}/align.txt')
    evidence += [f'{cold}/align.txt', f'{cold}/binaries.sha256']
    # The No/back-to-edit branch against its own original reference (dense capture and minimal control).
    NO = 'local/research/newgame-no-route-v2'
    no_route = {}
    if (ROOT / NO).exists():
        evidence += [f'{NO}/{n}' for n in ('binaries-check.txt', 'commands.txt', 'source-head.txt', 'tape-dense.txt',
                                           'tape-control.txt', 'summary.txt', 'suites.txt', 'python-suite.out',
                                           'ctest-release-newgame.out')]
        evidence += ['local/research/newgame-no-route-v1/NOTE.txt', 'local/research/newgame-no-route-v1/summary.txt',
                     'local/research/newgame-no-route-v1/no-then-yes-control-tape-USED-superseded.txt']
        for d in sorted(p for p in (ROOT / NO).iterdir() if p.is_dir()):
            rel = f'{NO}/{d.name}'
            evidence += [f'{rel}/{n}' for n in ('stdout.txt', 'reads-check.json', 'dense-compare.txt', 'dense-compare.json',
                                                 'control-exactness.json', 'report/route.json', 'report/name-frames.jsonl')]
            no_route[d.name] = {k: int(text(f'{rel}/{k}.exit' if k != 'exit' else f'{rel}/exit.txt'))
                                for k in ('exit', 'reads-check', 'dense-compare', 'control-exactness')}
            no_route[d.name]['dense_result'] = json.loads(text(f'{rel}/dense-compare.json'))['result']
    for d in ('local/research/newgame-final-suites-v1', 'local/research/newgame-final-suites-v2', 'local/research/newgame-pending-fix-v1',
              'local/research/newgame-postfix-sessions-v1', 'local/research/newgame-gates-v3', 'local/research/newgame-gates-v4', 'local/research/newgame-report-binding-v1'):
        if (ROOT / d).exists():
            evidence += [str(p.relative_to(ROOT)) for p in sorted((ROOT / d).rglob('*'))
                         if p.is_file() and ('/report/' not in str(p) and '/saves/' not in str(p)
                                             and not p.name.startswith('f.png'))]
    evidence += ['local/research/newgame-review-v1.sha256', 'local/research/newgame-review-v1/review.md',
                 'local/research/newgame-gate-audit-v1.sha256', 'local/research/newgame-gate-audit-v1/README.md',
                 'local/research/newgame-gate-audit-v1/my-gates.txt']
    manifest = {
        'schema': 'okage.newgame.validation.v1',
        'scope': SCOPE,
        'base_commit': '20b1c91',
        'binaries_built_from': text(f'{V}/bin/source-head.txt'),
        'head_commit_at_generation': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
        'elf_sha256': '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5',
        'stage_manifest_sha256_pinned': '1696227bf62b137522e12ea2f7529a0138f4ffeea449987901a140b6b57d7ad6',
        'original_reference': {'manifest': entry('docs/newgame-original-manifest.json'),
                               'analysis': 'local/research/newgame-original-v1/analysis',
                               'no_first_manifest': entry('docs/newgame-no-original-manifest.json'),
                               'no_first_evidence': 'local/research/newgame-no-original-v1'},
        'run_time_evidence': {
            'sessions': sessions, 'no_route_v2': no_route, 'restore_gate': restore, 'retire_routes_strict': retire, 'gates': gates, 'regressions': regressions,
            'ctest': {k: text(f'{V}/ctest-{k}.out').splitlines()[-3] for k in ('release-all', 'debug-all', 'sanitize-all')
                      if (ROOT / V / f'ctest-{k}.out').exists()},
            'python_suite': {n: [l for l in text(f'{V}/{n}').splitlines() if l.startswith(('Ran ', 'OK', 'FAILED'))]
                             for n in ('python-suite.out', 'python-suite-final.out') if (ROOT / V / n).exists()},
            'recheck_hardened': text(f'{V}/recheck-hardened/summary.txt').splitlines()
            if (ROOT / V / 'recheck-hardened/summary.txt').exists() else None,
        },
        'current_time_audit': {
            'stage': {k: v for k, v in json.loads(text(f'{F}/stage-audit-current-time.json')).items()
                      if isinstance(v, (int, float, str, bool))},
            'note': 're-hash of the stage when the evidence driver ran; it does not prove historical reads '
                    '(see sessions.*.input_check_exit for run-time reads)',
        },
        'tracked_sources': {p: sha(p) for p in sources if (ROOT / p).exists()},
        'evidence': [entry(p) for p in evidence],
        'commands': {'driver': f'NEWGAME_VALIDATION={V} tools/newgame_validation_run.sh {sub}', 'per_run': f'{F}/commands.txt',
                     'manifest': f'NEWGAME_VALIDATION={V} python3 tools/newgame_validation_manifest.py {sub} {out}'},
    }
    Path(out).write_text(json.dumps(manifest, indent=1) + '\n')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
