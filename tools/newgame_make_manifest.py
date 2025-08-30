#!/usr/bin/env python3
"""Write the hash-bound New Game original manifest.

usage: newgame_make_manifest.py EVIDENCE_DIR WORKTREE MAIN_CHECKOUT ORACLE_SOURCE_COMMIT OUT_JSON

Pins: ELF, disc, Play revision (read from the Play checkout's .git files), oracle source/binary and build settings,
every tracked source, the commands, every run's files and every analysis output (sha256 + bytes), and the result
digest the comparator checks (per-checkpoint resource-event/draw/VRAM-nibble counts and traversal t, plus the timing
anchors). Current-time audits (hashes recomputed now) are labelled separately from run-time provenance (what the runs
recorded). Refuses an existing OUT_JSON.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import restore_make_manifest as gm  # noqa: E402
import newgame_compare as cmp  # noqa: E402

PINNED_ELF = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
TRACKED = ('tools/play-oracle/startup.cpp', 'tools/play-oracle/CMakeLists.txt', 'tools/play-oracle/Attach.cmake',
           'tools/play-oracle/reference_input.hpp', 'tools/newgame_capture.sh', 'tools/newgame_original_extract.py',
           'tools/newgame_compare.py', 'tools/newgame_make_manifest.py', 'tests/test_newgame_compare.py',
           'tools/restore_original_extract.py', 'tools/restore_compare.py', 'tools/retire_title_original_extract.py',
           'tools/retire_title_compare.py', 'tools/title_original_extract.py')
RUNS = ('fill00-a', 'fill00-b', 'fillff-a', 'fillff-b', 'control-minimal-fill00', 'regress-retire-newgame', 'regress-title',
        'regress-default')
fsha, pin = gm.fsha, gm.pin

def main(argv):
    ev, wt, main_, commit, out = argv[:5]
    if os.path.exists(out):
        print('refusing existing manifest', file=sys.stderr)
        return 2
    elf = os.path.join(main_, 'local/extracted/SCUS_971.29')
    if fsha(elf) != PINNED_ELF:
        print('ELF is not the pinned SCUS_971.29', file=sys.stderr)
        return 2
    an = os.path.join(ev, 'analysis')
    ref = json.load(open(os.path.join(an, 'reference.json')))
    rev, ref_name = gm.play_revision(main_)
    cue = os.path.join(main_, 'local/disc/Okage - Shadow King (USA).cue')
    binf = os.path.join(main_, 'local/disc/Okage - Shadow King (USA).bin')
    rng = os.path.join(main_, 'local/research/rng-original-v1/analysis/rng.jsonl')
    retire_ev = os.path.join(main_, 'local/research/retire-title-original-v1')
    m = {
        'schema': 'okage.newgame-original-manifest.v1',
        'scope': "Independent original-execution reference of the FIRST cold title's New Game branch: the Retire newgame route continued past the 0x124548(mode 9) entry through name-entry construction (wrapper-owned bm_if atlas), an ordinary name edit (erase, select, append) and Yes, manager death and unlink, the published command {14,1,0} and request {2,72,0}, resource/wrapper retirement and frees, the first 0x133830 (Chapter 1 image list), its counter-240 fade request and actual fade completion, the command clear and request, chapter retirement and frees, stopping at the ENTRY of the second 0x133830 (command 0), before its body.",
        'oracle_source_commit': commit,
        'play': {'revision': rev, 'ref': ref_name, 'path': 'references/Play (main checkout, ignored)',
                 'tree_cleanliness': 'not verified by this tool'},
        'oracle': {
            'binary': 'build/newgame-oracle/okage-oracle/okage-startup-oracle',
            'retained_copy': os.path.relpath(os.path.join(ev, 'bin/okage-startup-oracle'), main_) + ' (ran every final run)',
            'retained_copy_sha256': fsha(os.path.join(ev, 'bin/okage-startup-oracle')),
            'configure': 'cmake -S <main>/references/Play -B build/newgame-oracle -G Ninja -DCMAKE_BUILD_TYPE=Debug '
                         '-DBUILD_PLAY=OFF -DBUILD_TESTS=ON -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 '
                         '-DCMAKE_PROJECT_Play_INCLUDE=<main>/tools/play-oracle/Attach.cmake -DDEBUGGER_INCLUDED=ON '
                         '-DOKAGE_REFERENCE_VULKAN=OFF -DOKAGE_REFERENCE_SPU_ADPCM_NDEBUG=ON',
            'build': 'ninja -C build/newgame-oracle okage-startup-oracle', 'cmake_build_type': 'Debug',
            'spu_adpcm_ndebug': 'Iop_SpuBase.cpp alone is compiled with NDEBUG: in a Debug build Play asserts '
                                'predictNumber < 5 on an ADPCM block the game plays after name entry (trial2, retained); '
                                'Play\'s own defined behaviour is a zero predictor row. No Play source is edited.',
            'mode': '--newgame-chapter (the Retire --title-retire newgame observer; Newgame hooks armed at the 0x124548 entry); '
                    '--newgame-chapter --minimal for the control'},
        'tracked_sources_sha256': {p: fsha(os.path.join(wt, p)) for p in TRACKED if os.path.exists(os.path.join(wt, p))},
        'inputs': {'elf': {'path': 'local/extracted/SCUS_971.29', 'sha256': PINNED_ELF},
                   'disc_cue': {'path': 'local/disc/Okage - Shadow King (USA).cue'},
                   'disc_bin': {'path': 'local/disc/Okage - Shadow King (USA).bin'},
                   'rng': {'path': 'local/research/rng-original-v1/analysis/rng.jsonl'},
                   'retire_evidence': 'local/research/retire-title-original-v1 (runs/newgame/*, analysis/newgame/successor-entry)',
                   'title_runs': 'local/research/title-original-v1/runs', 'startup_run': 'local/research/startup-original-v1/runs/fill00-a'},
        'current_time_audit': {
            'note': 'recomputed when this manifest was written; establishes the files present now, not historical reads',
            'elf': pin(elf), 'disc_cue': pin(cue), 'disc_bin': pin(binf), 'rng_jsonl': pin(rng),
            'retire_newgame_fill00_a_events': pin(os.path.join(retire_ev, 'runs/newgame/fill00-a/events.jsonl')),
            'retire_successor_entry_state': pin(os.path.join(retire_ev, 'analysis/newgame/successor-entry/state.json')),
            'retire_manifest': pin(os.path.join(wt, 'docs/retire-title-original-manifest.json')),
        },
        'run_time_provenance': {'note': 'from the runs themselves (run-meta.json, disc-opens.jsonl)', 'run_meta': {}},
        'fixed_settings': {
            'pad': 'ScriptedPad: neutral sticks; Retire newgame title script (Start, Left, confirm); then, decided only at '
                   '0x11a1c8 entries/returns: Circle (erase), Right, Cross (append), Start (End), Cross (Yes/No opens on '
                   'No), Up (Yes), Cross (accept); each set at the entry of a phase-1/-1 call and released at the return '
                   'of the call whose source state shows it consumed',
            'sound_handler': 'none', 'gs': 'null renderer + Title ledger', 'memory_cards': 'none (no card access on the route)',
            'vram_initial_fill': '0x00 (fill00-a/b, control, regressions) and 0xff (fillff-a/b)',
            'stop': 'ENTRY of the second 0x133830 (command 0, destination {2,72,0}), before its body',
            'checkpoints': 'the newgame ownership notes, section 2', 'deadline_seconds': 3600},
        'commands': [
            'OKAGE_PLAY_VM_LOCK=<main>/local/research/.play-vm.lock tools/newgame_capture.sh <evidence>/bin/okage-startup-oracle '
            '"<main>/local/disc/Okage - Shadow King (USA).cue" <main>/local/extracted/SCUS_971.29 <evidence> fill00-a:00: fill00-b:00: '
            'fillff-a:ff: fillff-b:ff: control-minimal-fill00:00:--newgame-chapter+--minimal regress-retire-newgame:00:--title-retire+newgame '
            'regress-title:00:--title regress-default:00:default',
            'python3 -I tools/newgame_original_extract.py <evidence> --out <evidence>/analysis --rng '
            '<main>/local/research/rng-original-v1/analysis/rng.jsonl --retire-evidence <main>/local/research/retire-title-original-v1 '
            '--regress-retire <evidence>/runs/regress-retire-newgame --regress-title <evidence>/runs/regress-title --regress-default '
            '<evidence>/runs/regress-default --title-runs <main>/local/research/title-original-v1/runs --startup-run '
            '<main>/local/research/startup-original-v1/runs/fill00-a > <evidence>/analysis.log',
            'python3 -I tools/newgame_compare.py <evidence>/analysis NATIVE_DIR [--json OUT] [--strict]',
            'python3 -I tools/newgame_make_manifest.py <evidence> <worktree> <main> <oracle_source_commit> docs/newgame-original-manifest.json'],
        'evidence_dir': os.path.relpath(ev, main_) + ' (read-only after capture)',
        'outputs': {}, 'runs': {}, 'result_digest': {'checkpoints': {}, 'anchors': {}},
        'limitations': [
            'Play research backend, not hardware; vblank numbers are observer-dependent (the minimal control takes fewer '
            'vblanks), traversal counts, RNG states, resource state and VRAM at the shared checkpoints are not',
            'null renderer: draw-written nibbles are conservative ledger rectangles; no original image or audio is captured',
            'the SPU ADPCM reader is built with NDEBUG (see oracle.spu_adpcm_ndebug); audio samples are not observed'],
    }
    for r in RUNS:
        p = os.path.join(ev, 'runs', r)
        if not os.path.isdir(p):
            continue
        files = {}
        for root, _, names in os.walk(p):
            for n in sorted(names):
                q = os.path.join(root, n)
                files[os.path.relpath(q, ev)] = pin(q)
        files['runs/' + r + '.log'] = pin(os.path.join(ev, 'runs', r + '.log'))
        m['runs'][r] = files
        m['run_time_provenance']['run_meta'][r] = json.load(open(os.path.join(p, 'run-meta.json')))
    for root, _, names in os.walk(an):
        for n in sorted(names):
            q = os.path.join(root, n)
            m['outputs']['analysis/' + os.path.relpath(q, an)] = pin(q)
    if os.path.exists(os.path.join(ev, 'analysis.log')):
        m['outputs']['analysis.log'] = pin(os.path.join(ev, 'analysis.log'))
    for cp in cmp.CHECKPOINTS:
        c = ref['checkpoints'][cp]
        m['result_digest']['checkpoints'][cp] = {'resource_events': c['resource_events'], 'rng_draws': c['rng_draws_before'],
                                                 'vram_known_nibbles': c['vram']['known_nibbles_required'],
                                                 'vram_unknown_nibbles': c['vram']['unknown_nibbles_required'],
                                                 'vb': c['vb'], 't': c['t']}
    nf = [json.loads(x) for x in open(os.path.join(an, 'name-frames.jsonl'))]
    cf = [json.loads(x) for x in open(os.path.join(an, 'chapter-frames.jsonl'))]
    m['result_digest']['anchors'] = {'checkpoint_t': {cp: ref['checkpoints'][cp]['t'] for cp in cmp.CHECKPOINTS},
                                     'name': cmp.name_anchors(nf), 'chapter': cmp.chapter_anchors(cf),
                                     'theme_end_t': ref['tails']['theme_end_t']}
    with open(out, 'x') as f:
        json.dump(m, f, indent=1)
        f.write('\n')
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
