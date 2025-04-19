#!/usr/bin/env python3
"""Write docs/rng-original-manifest.json: pins oracle binary, revisions, commands, inputs and output hashes.

usage: rng_make_manifest.py EVIDENCE_DIR WORKTREE MAIN_CHECKOUT SOURCE_COMMIT OUT.json
Reads files only; never modifies evidence. Refuses to overwrite OUT.json.
"""
import hashlib
import json
import os
import re
import sys

def fsha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 22), b''):
            h.update(chunk)
    return h.hexdigest()

def tree(root, skip=()):
    out = {}
    for dp, dn, fn in os.walk(root):
        dn.sort()
        for n in sorted(fn):
            p = os.path.join(dp, n)
            rel = os.path.relpath(p, root)
            if rel.split(os.sep)[0] in skip:
                continue
            out[rel] = {'sha256': fsha(p), 'bytes': os.path.getsize(p)}
    return out

def main():
    ev, wt, main_co, commit, out = sys.argv[1:6]
    if os.path.exists(out):
        sys.exit(f'refusing to overwrite {out}')
    cache = open(os.path.join(wt, 'build/rng-oracle/CMakeCache.txt')).read()

    def cv(k):
        m = re.search(r'^' + re.escape(k) + r'(?::[A-Z]+)?=(.*)$', cache, re.M)
        return m.group(1) if m else None
    play = os.path.join(main_co, 'references/Play')
    play_rev = open(os.path.join(play, '.git/refs/heads/master')).read().strip()
    disc_bin = os.path.join(main_co, 'local/disc/Okage - Shadow King (USA).bin')
    disc_report = json.load(open(os.path.join(wt, 'docs/disc-report.json')))
    ref = json.load(open(os.path.join(ev, 'analysis/reference.json')))
    efd = os.path.join(main_co, 'local/effect-input/CMNDATA/2D/TES.EFD')
    startup = os.path.join(main_co, 'local/research/startup-original-v1/runs/fill00-a')
    m = {
        'schema': 'okage.rng-original-manifest.v1',
        'scope': "Independent original-execution reference of every libc rand() call (0x1f14b0) in the cold-start prefix up to (not executing) EE PC 0x100618, and of its products (noise-grid cells, procedural template geometry, shape-cache owners, template request order).",
        'source_commit': commit,
        'base_commit': 'e709f21',
        'play': {'revision': play_rev, 'path': 'references/Play (main checkout, ignored)',
                 'tree_cleanliness': 'not verified (git on the Play checkout was not run from this isolated worktree)'},
        'oracle': {
            'binary': 'build/rng-oracle/okage-oracle/okage-startup-oracle (worktree)',
            'binary_sha256': fsha(os.path.join(wt, 'build/rng-oracle/okage-oracle/okage-startup-oracle')),
            'retained_copy': 'local/research/rng-original-v1/bin/okage-startup-oracle',
            'retained_copy_sha256': fsha(os.path.join(ev, 'bin/okage-startup-oracle')),
            'build_dir': 'build/rng-oracle (fresh; configured and built from scratch in the worktree)',
            'configure': 'cmake -S <main>/references/Play -B build/rng-oracle -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_PLAY=OFF '
                         '-DBUILD_TESTS=ON -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 '
                         '-DCMAKE_PROJECT_Play_INCLUDE=<worktree>/tools/play-oracle/Attach.cmake -DDEBUGGER_INCLUDED=ON '
                         '-DOKAGE_REFERENCE_VULKAN=OFF',
            'build': 'ninja -C build/rng-oracle okage-startup-oracle',
            'cmake_build_type': cv('CMAKE_BUILD_TYPE'), 'cxx_compiler': cv('CMAKE_CXX_COMPILER'),
            'osx_architectures': cv('CMAKE_OSX_ARCHITECTURES'), 'osx_sysroot': cv('CMAKE_OSX_SYSROOT'),
            'mode': '--rng-products (default Startup behaviour unchanged without the flag)',
        },
        'tracked_sources_sha256': {p: fsha(os.path.join(wt, p)) for p in (
            'tools/play-oracle/startup.cpp', 'tools/play-oracle/CMakeLists.txt', 'tools/play-oracle/Attach.cmake',
            'tools/rng_capture.sh', 'tools/rng_original_extract.py', 'tools/rng_compare.py',
            'tools/rng_make_manifest.py', 'tests/test_rng_compare.py')},
        'inputs': {
            'elf': {'path': 'local/extracted/SCUS_971.29', 'sha256': fsha(os.path.join(main_co, 'local/extracted/SCUS_971.29'))},
            'disc_cue': {'path': 'local/disc/Okage - Shadow King (USA).cue',
                         'sha256': fsha(os.path.join(main_co, 'local/disc/Okage - Shadow King (USA).cue'))},
            'disc_bin': {'path': 'local/disc/Okage - Shadow King (USA).bin', 'sha256': fsha(disc_bin),
                         'bytes': os.path.getsize(disc_bin), 'docs_disc_report_sha256': disc_report['image_sha256']},
            'contract_json': {'path': 'docs/startup-contract.json', 'sha256': fsha(os.path.join(wt, 'docs/startup-contract.json')),
                              'use': 'top-level step pcs only'},
            'functions_tsv': {'path': 'local/research/effect-template-v1/functions.tsv',
                              'sha256': fsha(os.path.join(main_co, 'local/research/effect-template-v1/functions.tsv')),
                              'use': 'caller-function spans of rand() call sites (Ghidra export of the pinned ELF)'},
            'tes_efd_as_read_by_original': ref['efd']['reads'],
            'tes_efd_owned_copy': {'path': 'local/effect-input/CMNDATA/2D/TES.EFD', 'sha256': fsha(efd),
                                   'use': 'diagnostic native run only'},
            'startup_cross_check_run': {'path': 'local/research/startup-original-v1/runs/fill00-a',
                                    'events_sha256': fsha(os.path.join(startup, 'events.jsonl')),
                                    'gs_ledger_sha256': fsha(os.path.join(startup, 'gs-ledger.jsonl')),
                                    'vram_sha256': fsha(os.path.join(startup, 'vram.bin'))},
            'play_config': 'fresh default configuration created by Play in <evidence>/cwd/rng-{a,b}/local/play-boot-config',
        },
        'fixed_settings': {
            'pad': 'RecordedPad with no input rows (neutral sticks)', 'sound_handler': 'none',
            'gs': 'null renderer sink with host-to-local writes + ledger', 'vram_initial_fill': '0x00 (both runs)',
            'stop': 'EE PC 0x100618 (jal 0x114f50), reached as a block start; the instruction has not executed',
            'deadline_seconds': 1500,
        },
        'commands': [
            'tools/rng_capture.sh <evidence>/bin/okage-startup-oracle "<main>/local/disc/Okage - Shadow King (USA).cue" '
            '<main>/local/extracted/SCUS_971.29 <evidence>   # as executed: bin/run_oracle-as-executed.sh <cwd> <out> <log>, '
            'rng-a then rng-b, identical oracle arguments',
            'python3 -I tools/rng_original_extract.py <evidence>/runs/rng-a <evidence>/runs/rng-b --contract '
            'docs/startup-contract.json --functions <main>/local/research/effect-template-v1/functions.tsv --startup '
            '<main>/local/research/startup-original-v1/runs/fill00-a --out <evidence>/analysis',
            'python3 -I tools/rng_compare.py <evidence>/analysis NATIVE_DIR [--json REPORT] [--noise-triangle-order multiset]',
            'diagnostic: build/rng-native-diag/okage-assets prepare-effect-templates <main>/local/effect-input/CMNDATA/2D/TES.EFD '
            '<main>/local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF 0xceeb6501 <evidence>/diagnostics/native-prep.bin '
            '(Release build of e709f21 native sources; copy retained as diagnostics/okage-assets-e709f21-release)',
            'diagnostic: cd <evidence> && python3 -I diagnostics/native_diag.py analysis diagnostics/native-prep.bin > '
            'diagnostics/native-diag.txt; python3 -I diagnostics/signed_zero_diag.py analysis diagnostics/native-prep.bin > '
            'diagnostics/signed-zero-diag.txt; python3 -I diagnostics/noise_diag.py analysis > diagnostics/noise-diag.txt',
            'python3 -I tools/rng_make_manifest.py <evidence> <worktree> <main> <source_commit> docs/rng-original-manifest.json',
            'OKAGE_MAIN_CHECKOUT=<main> python3 -m unittest discover -s tests',
        ],
        'evidence_dir': 'local/research/rng-original-v1 (read-only after capture)',
        'outputs': tree(ev, skip=('cwd',)),
        'auxiliary_play_config_files': tree(os.path.join(ev, 'cwd')),
        'result_digest': {
            'total_draws': ref['total_draws'],
            'phases': ref['phases'],
            'final_state': '0x%08x' % ref['final_state'],
            'template_runs': [{k: r[k] for k in ('def_id', 'source_elem', 'shape', 'factory', 'factory_address', 'first',
                                                  'draws', 'state_in', 'state_out')} for r in ref['template_runs']],
            'requests_total': ref['requests_total'], 'constructions': ref['constructions'],
            'procedural_templates': ref['procedural_templates'],
            'determinism': {k: v['identical'] for k, v in ref['determinism'].items()},
            'startup_cross_check': ref['startup_cross_check'],
        },
        'limitations': [
            'Play research backend, Debug build; not a physical PS2 proof. Play tree cleanliness not verified.',
            'Both runs use fill 0x00 and the same hooks; determinism is run-to-run equality under the same observer, plus '
            'equality of all 9,504 rand() entry states/return addresses, the GS ledger and VRAM with the earlier Startup capture '
            '(fewer hooks).',
            'Hooks perturb timing (96 vblanks at the stop vs 84 in Startup and 75 unhooked); the final EE RAM differs from Startup. '
            'No RNG value, product or resource state was observed to depend on it.',
            'Vertex floats are a documented mapping of the stored int16/u16/u8 packet data and strip order, not stored floats.',
            'ee-ram snapshots are the first 32 MiB of EE RAM only.',
        ],
    }
    with open(out, 'x') as f:
        json.dump(m, f, indent=1)
        f.write('\n')
    print('wrote', out)

if __name__ == '__main__':
    main()
