#!/usr/bin/env python3
"""Write docs/title-original-manifest.json: pins oracle binary, Play revision, commands, inputs and output hashes.

usage: title_make_manifest.py EVIDENCE_DIR WORKTREE MAIN_CHECKOUT ORACLE_SOURCE_COMMIT OUT.json
Reads files only; never modifies evidence. Refuses to overwrite OUT.json.
"""
import hashlib
import json
import os
import re
import sys

PLAY_REVISION = '83700b2c31e593bc94e845b4b31b797be84dda59'

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
    build = os.path.join(wt, 'build/title-oracle')
    cache = open(os.path.join(build, 'CMakeCache.txt')).read()

    def cv(k):
        m = re.search(r'^' + re.escape(k) + r'(?::[A-Z]+)?=(.*)$', cache, re.M)
        return m.group(1) if m else None
    play_rev = open(os.path.join(main_co, 'references/Play/.git/refs/heads/master')).read().strip()
    if play_rev != PLAY_REVISION:
        sys.exit(f'Play revision {play_rev} is not the pinned {PLAY_REVISION}')
    disc_bin = os.path.join(main_co, 'local/disc/Okage - Shadow King (USA).bin')
    disc_report = json.load(open(os.path.join(wt, 'docs/disc-report.json')))
    an = os.path.join(ev, 'analysis')
    ref = json.load(open(os.path.join(an, 'reference.json')))
    gw = json.load(open(os.path.join(an, 'gs-writers.json')))
    state = json.load(open(os.path.join(an, 'final-state.json')))
    tl = [json.loads(l) for l in open(os.path.join(an, 'events.jsonl'))]
    startup = os.path.join(main_co, 'local/research/startup-original-v1/runs/fill00-a')
    rng = os.path.join(main_co, 'local/research/rng-original-v1/runs/rng-a')
    built = fsha(os.path.join(build, 'okage-oracle/okage-startup-oracle'))
    kept = fsha(os.path.join(ev, 'bin/okage-startup-oracle'))
    if built != kept:
        sys.exit('retained oracle copy differs from the build output')
    kinds = sorted(set(e['kind'] for e in tl) - {'phase'})
    m = {
        'schema': 'okage.title-original-manifest.v1',
        'scope': 'Independent original-execution reference of the cold-boot texture/handle lifecycle from power-on through '
                 'storage readiness, both logos, logo resource retirement and title construction, stopping at the RETURN of '
                 '0x12c350 with selector 1 (paused at its return address, before the first 0x1296f8 title update). '
                 'Unchanged retail SCUS_971.29 under the Play research VM, neutral pad. Research evidence only; not physical '
                 'PS2 proof; never product input.',
        'oracle_source_commit': commit,
        'base_commit': '5badd42',
        'play': {'revision': play_rev, 'path': 'references/Play (main checkout, ignored)',
                 'tree_cleanliness': 'not verified (git on the Play checkout was not run from the isolated worktree)'},
        'oracle': {
            'binary': 'build/title-oracle/okage-oracle/okage-startup-oracle (worktree)',
            'binary_sha256': built,
            'retained_copy': 'local/research/title-original-v1/bin/okage-startup-oracle (the binary that ran)',
            'retained_copy_sha256': kept,
            'build_dir': 'build/title-oracle (fresh; configured and built from scratch in the worktree)',
            'configure': 'cmake -S <main>/references/Play -B build/title-oracle -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_PLAY=OFF '
                         '-DBUILD_TESTS=ON -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 '
                         '-DCMAKE_PROJECT_Play_INCLUDE=<worktree>/tools/play-oracle/Attach.cmake -DDEBUGGER_INCLUDED=ON '
                         '-DOKAGE_REFERENCE_VULKAN=OFF',
            'build': 'ninja -C build/title-oracle okage-startup-oracle',
            'cmake_build_type': cv('CMAKE_BUILD_TYPE'), 'cxx_compiler': cv('CMAKE_CXX_COMPILER'),
            'osx_architectures': cv('CMAKE_OSX_ARCHITECTURES'), 'osx_sysroot': cv('CMAKE_OSX_SYSROOT'),
            'mode': '--title (implies the RNG --rng-products observations); --title --minimal for the control run. '
                    'Default Startup mode re-verified byte-identical to local/research/startup-original-v1/runs/fill00-a '
                    '(events, GS ledger, disc opens, VRAM, final state, EE RAM) with this source.',
        },
        'tracked_sources_sha256': {p: fsha(os.path.join(wt, p)) for p in (
            'tools/play-oracle/startup.cpp', 'tools/play-oracle/CMakeLists.txt', 'tools/play-oracle/Attach.cmake',
            'tools/title_capture.sh', 'tools/title_original_extract.py', 'tools/title_compare.py',
            'tools/title_make_manifest.py', 'tests/test_title_compare.py', 'docs/title-original-report.md') if p != 'docs/title-original-report.md' or os.path.exists(os.path.join(wt, p))},
        'inputs': {
            'elf': {'path': 'local/extracted/SCUS_971.29', 'sha256': fsha(os.path.join(main_co, 'local/extracted/SCUS_971.29'))},
            'disc_cue': {'path': 'local/disc/Okage - Shadow King (USA).cue',
                         'sha256': fsha(os.path.join(main_co, 'local/disc/Okage - Shadow King (USA).cue'))},
            'disc_bin': {'path': 'local/disc/Okage - Shadow King (USA).bin', 'sha256': fsha(disc_bin),
                         'bytes': os.path.getsize(disc_bin), 'docs_disc_report_sha256': disc_report['image_sha256']},
            'files_read_by_the_original': ref['file_reads'],
            'startup_cross_check_run': {'path': 'local/research/startup-original-v1/runs/fill00-a',
                                    'final_state_sha256': fsha(os.path.join(startup, 'final-state.jsonl')),
                                    'events_sha256': fsha(os.path.join(startup, 'events.jsonl'))},
            'rng_cross_check_run': {'path': 'local/research/rng-original-v1/runs/rng-a',
                                    'events_sha256': fsha(os.path.join(rng, 'events.jsonl'))},
            'play_config': 'fresh default configuration (and memory card directory) created by Play in '
                           '<evidence>/cwd/<run>/local/play-boot-config',
        },
        'fixed_settings': {
            'pad': 'RecordedPad with no input rows (neutral sticks, no buttons)', 'sound_handler': 'none',
            'gs': 'null renderer (no rasterisation) + ledger: host-to-local writes performed by Play; primitives, '
                  'local-to-local and CLUT loads classified, never executed',
            'vram_initial_fill': '0x00 (fill00-a/b, control) and 0xff (fillff-a/b), applied before the first guest instruction',
            'stop': 'return address of 0x12c350 when its selector (obj+0x24) is 1; the VM stays paused there (EE PC 0x11528c)',
            'deadline_seconds': 5400,
        },
        'commands': [
            'OKAGE_PLAY_VM_LOCK=<main>/local/research/.play-vm.lock tools/title_capture.sh <evidence>/bin/okage-startup-oracle '
            '"<main>/local/disc/Okage - Shadow King (USA).cue" <main>/local/extracted/SCUS_971.29 <evidence>   # as executed: the '
            'same five oracle invocations issued one at a time under the lock (cd <evidence>/cwd/<run> && <oracle> DISC ELF '
            '<evidence>/runs/<run> 00|ff --deadline-seconds 5400 --title [--minimal] > <evidence>/runs/<run>.log 2>&1)',
            'python3 -I tools/title_original_extract.py <evidence>/runs --out <evidence>/analysis --startup '
            '<main>/local/research/startup-original-v1/runs/fill00-a --rng <main>/local/research/rng-original-v1/runs/rng-a '
            '> <evidence>/analysis.log',
            'python3 -I tools/title_compare.py <evidence>/analysis NATIVE_REPORT_DIR [--json OUT]',
            'python3 -I tools/title_make_manifest.py <evidence> <worktree> <main> <oracle_source_commit> docs/title-original-manifest.json',
            'OKAGE_MAIN_CHECKOUT=<main> python3 -m unittest discover -s tests',
        ],
        'evidence_dir': 'local/research/title-original-v1 (read-only after capture)',
        'outputs': tree(ev, skip=('cwd',)),
        'auxiliary_play_config_files': tree(os.path.join(ev, 'cwd')),
        'result_digest': {
            'resource_events': sum(1 for e in tl if e['kind'] != 'phase'),
            'resource_events_by_kind': {k: sum(1 for e in tl if e['kind'] == k) for k in kinds},
            'live_allocations': len(state['allocations']), 'handles': len(state['handles']), 'records': len(state['records']),
            'image_list': state['image_list'], 'materials': len(state['materials']),
            'vram_known_nibbles': gw['vram']['known_nibbles_required'],
            'vram_unknown_nibbles': gw['vram']['unknown_nibbles_required'],
            'vram_stats': gw['vram']['stats'],
            'gs': {'host_to_local': gw['host_to_local']['count'], 'draw_classes': len(gw['draw_classes']),
                   'primitives': gw['summary']['primitives'], 'local_to_local': len(gw['local_to_local']),
                   'clut_loads': gw['gs_summary']['clut_transfers'], 'flips': gw['summary']['flips']},
            'mode8_constructions': [{k: c[k] for k in ('selector', 'enter_vb', 'return_vb')} for c in ref['mode8_constructions']],
            'logo_ticks': ref['logo_ticks'], 'storage_events': ref['storage_events'],
            'rand_calls_after_prefix': ref['rand_after_prefix']['calls'],
            'determinism': {k: v['identical'] for k, v in ref['determinism'].items() if isinstance(v, dict) and 'identical' in v},
            'startup_cross_check': ref['startup_cross_check'], 'rng_cross_check': ref['rng_cross_check'],
            'control_minimal': ref['control_minimal'],
        },
        'limitations': [
            'Play research backend, Debug build; not a physical PS2 proof. Play tree cleanliness not verified.',
            'The null renderer rasterises nothing: draw/local-to-local "writes" are ledger classifications (conservative '
            'rectangles), not observed memory contents. The required-unknown set is therefore a conservative superset of '
            'real coverage (vertex bounding boxes clipped to SCISSOR, FBMSK/ZMSK/TEST-NEVER honoured).',
            'Written-lane masks are paired-fill derived and cross-checked against the ledger upload footprint (equal).',
            'Play PSMCT24 host-to-local transfers read 3-byte pixels per chunk; every PSMCT24 chunk here was a multiple of 3 '
            'bytes (ledger ct24_chunks_not_multiple_of_3 = 0), so no chunk-boundary artefact applies.',
            'ee_event fields in gs-ledger.jsonl are an approximate host-thread cross-reference and are excluded from '
            'determinism; every other ledger field is identical across the four runs.',
            'Hooks perturb timing (stop at 843 vblanks hooked, 822 in the minimal control); state, VRAM and selectors agree.',
            'ee-ram snapshots are the first 32 MiB of EE RAM only.',
        ],
    }
    with open(out, 'x') as f:
        json.dump(m, f, indent=1)
        f.write('\n')
    print('wrote', out)

if __name__ == '__main__':
    main()
