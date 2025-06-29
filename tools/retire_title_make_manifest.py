#!/usr/bin/env python3
"""Write docs/retire-title-original-manifest.json from a finished Retire evidence directory.

usage: retire_title_make_manifest.py EVIDENCE_DIR WORKTREE MAIN_CHECKOUT ORACLE_SOURCE_COMMIT OUT_JSON

Run-time provenance (what ran: binary, commands, run-meta, logs, capture hashes) is recorded separately from the
current-time audit (hashes of inputs and pinned prior evidence recomputed now; they establish what is present
now, not what was read historically). Reads only, except OUT_JSON (must not exist).
"""
import hashlib
import json
import os
import sys

TRACKED = ('tools/play-oracle/startup.cpp', 'tools/play-oracle/CMakeLists.txt', 'tools/play-oracle/Attach.cmake',
           'tools/play-oracle/reference_input.hpp', 'tools/retire_title_capture.sh', 'tools/retire_title_original_extract.py',
           'tools/title_original_extract.py', 'tools/retire_title_compare.py', 'tools/retire_title_make_manifest.py',
           'tests/test_retire_title_compare.py')
ROUTES = ('restore', 'newgame')
CPS = ('first-update', 'stable-menu', 'removal-request', 'resource-retirement', 'successor-entry')

def fsha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()

def pin(p):
    return {'sha256': fsha(p), 'bytes': os.path.getsize(p)}

def tree(root, rel):
    out = {}
    base = os.path.join(root, rel)
    for d, _, fs in sorted(os.walk(base)):
        for f in sorted(fs):
            p = os.path.join(d, f)
            out[os.path.relpath(p, root)] = pin(p)
    return out

def main(argv):
    ev, wt, main_, commit, out = argv
    if os.path.exists(out):
        print('refusing existing output', file=sys.stderr)
        return 2
    a = os.path.join(ev, 'analysis')
    refs = {r: json.load(open(os.path.join(a, r, 'reference.json'))) for r in ROUTES}
    reg = json.load(open(os.path.join(a, 'regression.json'))) if os.path.exists(os.path.join(a, 'regression.json')) else None
    digest = {'routes': {}}
    for r in ROUTES:
        cps = refs[r]['checkpoints']
        digest['routes'][r] = {cp: {'resource_events': cps[cp]['resource_events'], 'rng_draws': cps[cp]['rng_draws_before'],
                                    'vram_known_nibbles': cps[cp]['vram']['known_nibbles_required'],
                                    'vram_unknown_nibbles': cps[cp]['vram']['unknown_nibbles_required'],
                                    'events_by_kind': cps[cp]['events_by_kind'], 'vb': cps[cp]['vb'],
                                    'title_n': cps[cp]['title_n'], 'state': cps[cp]['state'],
                                    'effect106': cps[cp]['effect106']} for cp in CPS}
        digest['routes'][r]['title-constructed'] = {k: cps['title-constructed'][k] for k in
                                                    ('vb', 'title_n', 'resource_events', 'events_by_kind')}
        digest['routes'][r]['pad_script'] = refs[r]['pad']['script_actions']
        digest['routes'][r]['nonneutral_pad_reads'] = refs[r]['pad']['nonneutral_reads']
        digest['routes'][r]['title_submode_changes'] = refs[r]['title']['submode_changes']
        digest['routes'][r]['title_selection_changes'] = refs[r]['title']['selection_changes']
        digest['routes'][r]['effect106_events'] = [{k: e[k] for k in e if k in ('op', 'slot', 'shape', 'element', 'handle', 'vb',
                                                                                 'title_n', 'caller_ra')}
                                                   for e in refs[r]['effect106']['events']]
        digest['routes'][r]['mode_events'] = [{k: e[k] for k in e if k in ('op', 'mode', 'selector', 'arg', 'node', 'vb', 'title_n')}
                                              for e in refs[r]['mode_events']]
        digest['routes'][r]['rng'] = {k: refs[r]['rng'][k] for k in ('draws_total', 'startup_draws_rng', 'state_at_stop')} | {
            'draws_after_prefix': len(refs[r]['rng']['draws_after_prefix'])}
        digest['routes'][r]['survivors_at_successor_entry'] = refs[r]['survivors_at_successor_entry']
        digest['routes'][r]['pending_at_stop'] = refs[r]['pending_at_stop']
        digest['routes'][r]['retirement'] = {k: refs[r]['retirement'][k] for k in
                                             ('retire_callers', 'model_table_retire_order', 'image_list_retire_order',
                                              'nightv_table_ptr', 'texture_events_after_resource_child_return', 'vb')}
        digest['routes'][r]['determinism_all_identical'] = all(v['identical'] for k, v in refs[r]['determinism'].items()
                                                               if isinstance(v, dict) and 'identical' in v)
        digest['routes'][r]['control_minimal'] = refs[r].get('control_minimal')
        digest['routes'][r]['title_neutrality'] = refs[r].get('title_neutrality')
    outputs = tree(ev, 'analysis')
    runs = tree(ev, 'runs')
    m = {
        'schema': 'okage.retire.title-original-manifest.v1',
        'scope': "Independent original-execution reference of the cold-boot title lifecycle continued past the Title stop (0x12c350 selector-1 return) through title updates, the Start/menu interaction, effect106 (water) creation/binding/removal request, title resource retirement, mode-8 destruction and wrapper frees, stopping at the ENTRY of 0x124548 (mode9/mode10 successor constructor) before its body executes, for the routes restore (Start, confirm default Restore) and newgame (Start, Left, confirm New Game).",
        'oracle_source_commit': commit,
        'play': {'revision': open(os.path.join(main_, 'references/Play/.git/refs/heads/master')).read().strip(),
                 'path': 'references/Play (main checkout, ignored)',
                 'tree_cleanliness': 'not verified (git on the Play checkout is outside the isolated worktree)'},
        'oracle': {
            'binary': 'build/retire-title-oracle/okage-oracle/okage-startup-oracle (worktree)',
            'retained_copy': 'local/research/retire-title-original-v1/bin/okage-startup-oracle (the binary that ran every final run)',
            'retained_copy_sha256': fsha(os.path.join(ev, 'bin/okage-startup-oracle')),
            'configure': 'cmake -S <main>/references/Play -B build/retire-title-oracle -G Ninja -DCMAKE_BUILD_TYPE=Debug '
                         '-DBUILD_PLAY=OFF -DBUILD_TESTS=ON -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 '
                         '-DCMAKE_PROJECT_Play_INCLUDE=<worktree>/tools/play-oracle/Attach.cmake -DDEBUGGER_INCLUDED=ON '
                         '-DOKAGE_REFERENCE_VULKAN=OFF',
            'build': 'ninja -C build/retire-title-oracle okage-startup-oracle',
            'cmake_build_type': 'Debug',
            'mode': '--title-retire restore|newgame (implies the --title and RNG --rng-products observations plus the Retire '
                    'hooks); --title-retire ROUTE --minimal for the control (only 0x1296f8 and 0x124548 entries). Default '
                    'and --title modes unchanged: see regression.',
        },
        'tracked_sources_sha256': {p: fsha(os.path.join(wt, p)) for p in TRACKED if os.path.exists(os.path.join(wt, p))},
        'inputs': {
            'elf': {'path': 'local/extracted/SCUS_971.29', 'sha256': '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'},
            'disc_cue': {'path': 'local/disc/Okage - Shadow King (USA).cue'},
            'disc_bin': {'path': 'local/disc/Okage - Shadow King (USA).bin'},
            'rng': {'path': 'local/research/rng-original-v1/analysis/rng.jsonl'},
            'title_runs': 'local/research/title-original-v1/runs', 'title_analysis': 'local/research/title-original-v1/analysis',
            'startup_run': 'local/research/startup-original-v1/runs/fill00-a',
        },
        'current_time_audit': {
            'note': 'recomputed when this manifest was written; establishes the files present now, not historical reads',
            'elf_sha256': fsha(os.path.join(main_, 'local/extracted/SCUS_971.29')),
            'disc_cue': pin(os.path.join(main_, 'local/disc/Okage - Shadow King (USA).cue')),
            'disc_bin': pin(os.path.join(main_, 'local/disc/Okage - Shadow King (USA).bin')),
            'rng_jsonl': pin(os.path.join(main_, 'local/research/rng-original-v1/analysis/rng.jsonl')),
            'title_fill00_a_final_state': pin(os.path.join(main_, 'local/research/title-original-v1/runs/fill00-a/final-state.jsonl')),
            'title_analysis_events': pin(os.path.join(main_, 'local/research/title-original-v1/analysis/events.jsonl')),
            'startup_fill00_a_events': pin(os.path.join(main_, 'local/research/startup-original-v1/runs/fill00-a/events.jsonl')),
        },
        'run_time_provenance': {
            'note': 'from the runs themselves (run-meta.json, logs, disc-opens.jsonl with the sha256 of every file the '
                    'guest opened, as read by the VM at run time)',
            'run_meta': {f'{r}/{n}': json.load(open(os.path.join(ev, 'runs', r, n, 'run-meta.json')))
                         for r in ROUTES for n in ('fill00-a', 'fill00-b', 'fillff-a', 'fillff-b', 'control-minimal-fill00')},
            'regression_run_meta': {n: json.load(open(os.path.join(ev, 'runs/regression', n, 'run-meta.json')))
                                    for n in ('title-fill00', 'default-fill00')
                                    if os.path.exists(os.path.join(ev, 'runs/regression', n, 'run-meta.json'))},
        },
        'fixed_settings': {
            'pad': 'ScriptedPad (tools/play-oracle/startup.cpp): neutral sticks (128), buttons set by the observer only while '
                   'paused at a 0x1296f8 entry, from guest state; applied at the next PadHandler::Update like RecordedPad',
            'pad_script_restore': 'submode 10 -> press Start; Start consumed (pressed word & 0x800 at a submode-10 entry) -> '
                                  'release; first submode-0x1e entry (selection 1) -> press Cross (0x1f85dc = 0x40); confirm '
                                  'consumed -> release',
            'pad_script_newgame': 'as restore until the first submode-0x1e entry -> press Left (0x1f8604 = 0x8000 sets 0x259970, '
                                  'which decrements the selection; Down sets 0x25996c, which this menu does not read); Left '
                                  'consumed (0x259970 = 1) -> release; selection 0 and 0x259970 = 0 -> press Cross; confirm '
                                  'consumed -> release',
            'sound_handler': 'none', 'gs': 'null renderer + Title ledger',
            'vram_initial_fill': '0x00 (fill00-a/b, control) and 0xff (fillff-a/b), before the first guest instruction',
            'stop': 'ENTRY of 0x124548 with parent+0x4c = 10 (restore) / 9 (newgame); VM paused there',
            'checkpoints': 'title-constructed (0x12c350 selector-1 return = the Title stop), first-update (entry of the first '
                           '0x1296f8), stable-menu (the 0x1296f8 entry whose pressed word carries the consumed Start), '
                           'removal-request (entry of 0x103640 for the water slot from 0x1296f8), resource-retirement (entry '
                           'of 0x12c8d8 = after the title resource child 0x101058 returned, before the mode-8 destructor '
                           'body), successor-entry (entry of 0x124548)',
            'deadline_seconds': 3600,
        },
        'commands': [
            'OKAGE_PLAY_VM_LOCK=<main>/local/research/.play-vm.lock tools/retire_title_capture.sh <evidence>/bin/okage-startup-oracle '
            '"<main>/local/disc/Okage - Shadow King (USA).cue" <main>/local/extracted/SCUS_971.29 <evidence>',
            'sh <evidence>/regress-as-executed.sh   # one --title and one default-mode run of the same binary, fill 00, under the lock',
            'python3 -I tools/retire_title_original_extract.py <evidence> --out <evidence>/analysis --rng '
            '<main>/local/research/rng-original-v1/analysis/rng.jsonl --title-runs <main>/local/research/title-original-v1/runs '
            '--title-analysis <main>/local/research/title-original-v1/analysis --startup-run '
            '<main>/local/research/startup-original-v1/runs/fill00-a --regress-title <evidence>/runs/regression/title-fill00 '
            '--regress-default <evidence>/runs/regression/default-fill00 > <evidence>/analysis.log',
            'python3 -I tools/retire_title_compare.py <evidence>/analysis NATIVE_DIR [--json OUT]',
            'python3 -I tools/retire_title_make_manifest.py <evidence> <worktree> <main> <oracle_source_commit> docs/retire-title-original-manifest.json',
            'OKAGE_MAIN_CHECKOUT=<main> python3 -m unittest discover -s tests',
        ],
        'evidence_dir': 'local/research/retire-title-original-v1 (read-only after capture; trials/ and bin-trial*/ retained '
                        'development runs of earlier observer builds, not used by the analysis)',
        'regression': reg,
        'outputs': outputs,
        'runs': runs,
        'retained_other': {k: v for k, v in {**tree(ev, 'trials'), **tree(ev, 'bin-trial1'), **tree(ev, 'bin-trial2')}.items()
                           if not k.endswith('.bin')},
        'result_digest': {'elf_sha256': '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5', **digest},
        'expected_coverage': {
            'checkpoints': list(CPS),
            'event_kinds': ['alloc', 'upload', 'ref', 'free', 'handle', 'retire', 'bind', 'name', 'reserve', 'material',
                            'effect:create', 'effect:bind', 'effect:remove-request', 'effect:reclaim', 'mode:request',
                            'mode:resource-retire', 'mode:destroy', 'mode:wrapper-free'],
            'not_reached_in_this_window': ['effect:reclaim (0x102218 never services the water job after the resource child '
                                           'is retired; pending at the stop)', 'texture allocation or upload after the Title stop',
                                           'rand() draws after 0x100618', 'local-to-local GS transfers'],
        },
        'limitations': [
            'Play research backend, Debug build; not a physical PS2 proof. Play tree cleanliness not verified.',
            'Vblank numbers are observer-dependent (hooked successor entry vb 1036 restore / 1039 newgame vs 1013 in the minimal '
            'restore control); title-update ordinals, pad words and resource state are not.',
            'The null renderer rasterises nothing: draw-written nibbles are ledger classifications (conservative rectangles).',
            'Checkpoint VRAM is read after submitting the processed GS register writes and waiting for the GS thread; the '
            'last host-to-local transfer record stays open in the ledger (complete: received == expected).',
            'The pad script is state-driven: Start is pressed at the first accepting title update, so the title fade-in has '
            'barely begun at stable-menu; idle title behaviour (attract timeout 1500 updates) is not exercised.',
            'ee-ram snapshots are the first 32 MiB of EE RAM only and only at the stop.',
        ],
    }
    with open(out, 'x') as f:
        json.dump(m, f, indent=1)
        f.write('\n')
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
