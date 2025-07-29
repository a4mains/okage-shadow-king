#!/usr/bin/env python3
"""Write the hash-bound Restore-cancel original manifest.

usage: restore_make_manifest.py EVIDENCE_DIR WORKTREE MAIN_CHECKOUT ORACLE_SOURCE_COMMIT OUT_JSON

Pins: ELF, disc, Play revision (read from the Play checkout's .git files), oracle source/binary, every tracked source,
the isolated memory-card setup of every run (memcards-before/after.json), the commands, every run's files and every
analysis output (sha256 + bytes), and the result digest the comparator checks. Current-time audits (hashes recomputed
now) are labelled separately from run-time provenance (what the runs recorded). Refuses an existing OUT_JSON.
"""
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import restore_compare as cmp  # noqa: E402

TRACKED = ('tools/play-oracle/startup.cpp', 'tools/play-oracle/CMakeLists.txt', 'tools/play-oracle/Attach.cmake',
           'tools/play-oracle/reference_input.hpp', 'tools/restore_capture.sh', 'tools/restore_original_extract.py',
           'tools/restore_compare.py', 'tools/restore_make_manifest.py', 'tests/test_restore_compare.py',
           'tools/retire_title_original_extract.py', 'tools/retire_title_compare.py', 'tools/title_original_extract.py',
           'docs/restore-original-report.md')
RUNS = ('fill00-a', 'fill00-b', 'fillff-a', 'fillff-b', 'control-minimal-fill00', 'regress-retire-restore', 'regress-title',
        'regress-default')

def fsha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()

def pin(p):
    return {'sha256': fsha(p), 'bytes': os.path.getsize(p)}

def play_revision(main):
    g = os.path.join(main, 'references/Play/.git')
    try:
        head = open(os.path.join(g, 'HEAD')).read().strip()
        if head.startswith('ref: '):
            ref = head[5:]
            p = os.path.join(g, ref)
            if os.path.exists(p):
                return open(p).read().strip(), ref
            for line in open(os.path.join(g, 'packed-refs')):
                if line.strip().endswith(' ' + ref):
                    return line.split()[0], ref
        return head, 'detached'
    except OSError as exc:
        return None, str(exc)

PINNED_ELF = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'

def main(argv):
    ev, wt, main_, commit, out = argv
    if os.path.exists(out):
        print('refusing existing manifest', file=sys.stderr)
        return 2
    if fsha(os.path.join(main_, 'local/extracted/SCUS_971.29')) != PINNED_ELF:
        print('ELF is not the pinned SCUS_971.29', file=sys.stderr)
        return 2
    an = os.path.join(ev, 'analysis')
    ref = json.load(open(os.path.join(an, 'reference.json')))
    rev, ref_name = play_revision(main_)
    elf = os.path.join(main_, 'local/extracted/SCUS_971.29')
    cue = os.path.join(main_, 'local/disc/Okage - Shadow King (USA).cue')
    binf = os.path.join(main_, 'local/disc/Okage - Shadow King (USA).bin')
    rng = os.path.join(main_, 'local/research/rng-original-v1/analysis/rng.jsonl')
    retire_ev = os.path.join(main_, 'local/research/retire-title-original-v1')
    m = {
        'schema': 'okage.restore-original-manifest.v1',
        'scope': "Independent original-execution reference of the cold-boot Retire Restore route continued past the 0x124548 entry through Restore construction, the scan of an isolated empty research memory card, the ready empty-slot UI, one ordinary Cancel, window death, manager unlink, resource retirement, wrapper cleanup and free, the mode request back to the title and the return of the second 0x12c350(selector 1).",
        'oracle_source_commit': commit,
        'play': {'revision': rev, 'ref': ref_name, 'path': 'references/Play (main checkout, ignored)',
                 'tree_cleanliness': 'not verified (git on the Play checkout is outside the isolated worktree)'},
        'oracle': {
            'binary': 'build/restore-oracle/okage-oracle/okage-startup-oracle (worktree)',
            'retained_copy': 'local/research/restore-original-v1/bin/okage-startup-oracle (ran every final run)',
            'retained_copy_sha256': fsha(os.path.join(ev, 'bin/okage-startup-oracle')),
            'configure': 'cmake -S <main>/references/Play -B build/restore-oracle -G Ninja -DCMAKE_BUILD_TYPE=Debug '
                         '-DBUILD_PLAY=OFF -DBUILD_TESTS=ON -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 '
                         '-DCMAKE_PROJECT_Play_INCLUDE=<worktree>/tools/play-oracle/Attach.cmake -DDEBUGGER_INCLUDED=ON '
                         '-DOKAGE_REFERENCE_VULKAN=OFF',
            'build': 'ninja -C build/restore-oracle okage-startup-oracle', 'cmake_build_type': 'Debug',
            'mode': '--restore-cancel --memcards DIR (the Retire --title-retire restore observer; Restore hooks armed at the '
                    '0x124548 entry); --restore-cancel --memcards DIR --minimal for the control'},
        'tracked_sources_sha256': {p: fsha(os.path.join(wt, p)) for p in TRACKED if p != 'docs/restore-original-report.md' or os.path.exists(os.path.join(wt, p))},
        'inputs': {'elf': {'path': 'local/extracted/SCUS_971.29', 'sha256': PINNED_ELF},
                   'disc_cue': {'path': 'local/disc/Okage - Shadow King (USA).cue'},
                   'disc_bin': {'path': 'local/disc/Okage - Shadow King (USA).bin'},
                   'rng': {'path': 'local/research/rng-original-v1/analysis/rng.jsonl'},
                   'retire_evidence': 'local/research/retire-title-original-v1 (runs/restore/*, analysis/restore/successor-entry)',
                   'title_runs': 'local/research/title-original-v1/runs', 'startup_run': 'local/research/startup-original-v1/runs/fill00-a'},
        'current_time_audit': {
            'note': 'recomputed when this manifest was written; establishes the files present now, not historical reads',
            'elf': pin(elf), 'disc_cue': pin(cue), 'disc_bin': pin(binf), 'rng_jsonl': pin(rng),
            'retire_restore_fill00_a_events': pin(os.path.join(retire_ev, 'runs/restore/fill00-a/events.jsonl')),
            'retire_successor_entry_state': pin(os.path.join(retire_ev, 'analysis/restore/successor-entry/state.json')),
            'retire_manifest': pin(os.path.join(wt, 'docs/retire-title-original-manifest.json')),
        },
        'card_setup': {
            'rule': 'per run a fresh EVIDENCE/cards/<run>/{mc0,mc1}, created empty by tools/restore_capture.sh; the '
                    'oracle refuses a non-empty or symlinked card, sets Play ps2.mc0/mc1.directory.v2 to them and lists them '
                    'before and after the run; no user card or save directory is referenced',
            'runs': {}},
        'run_time_provenance': {'note': 'from the runs themselves (run-meta.json, memcards-*.json, disc-opens.jsonl)', 'run_meta': {}},
        'fixed_settings': {
            'pad': 'ScriptedPad: neutral sticks; Retire restore title script; then Circle at the first 0x11e480 entry with state '
                   '0x21c and the input gate open (opacity == *0x24ecc4, != 0), released at the return of the call that '
                   'consumed it (0x21c -> -1)',
            'sound_handler': 'none', 'gs': 'null renderer + Title ledger',
            'vram_initial_fill': '0x00 (fill00-a/b, control, regressions) and 0xff (fillff-a/b)',
            'stop': 'RETURN of the second 0x12c350(selector 1) (0x11528c), before its first 0x1296f8',
            'checkpoints': 'the Restore checkpoint contract', 'deadline_seconds': 3600},
        'commands': [
            'OKAGE_PLAY_VM_LOCK=<main>/local/research/.play-vm.lock tools/restore_capture.sh <evidence>/bin/okage-startup-oracle '
            '"<main>/local/disc/Okage - Shadow King (USA).cue" <main>/local/extracted/SCUS_971.29 <evidence> fill00-a:00: fill00-b:00: '
            'fillff-a:ff: fillff-b:ff: control-minimal-fill00:00:--restore-cancel+--minimal regress-retire-restore:00:--title-retire+restore '
            'regress-title:00:--title regress-default:00:default',
            'python3 -I tools/restore_original_extract.py <evidence> --out <evidence>/analysis --rng '
            '<main>/local/research/rng-original-v1/analysis/rng.jsonl --retire-evidence <main>/local/research/retire-title-original-v1 '
            '--regress-retire <evidence>/runs/regress-retire-restore --regress-title <evidence>/runs/regress-title --regress-default '
            '<evidence>/runs/regress-default --title-runs <main>/local/research/title-original-v1/runs --startup-run '
            '<main>/local/research/startup-original-v1/runs/fill00-a > <evidence>/analysis.log',
            'python3 -I tools/restore_compare.py <evidence>/analysis NATIVE_DIR [--json OUT] [--strict]',
            'python3 -I tools/restore_make_manifest.py <evidence> <worktree> <main> <oracle_source_commit> docs/restore-original-manifest.json',
            'OKAGE_MAIN_CHECKOUT=<main> python3 -m unittest discover -s tests'],
        'evidence_dir': 'local/research/restore-original-v1 (read-only after capture; trials/ holds the development runs '
                        'of earlier observer builds, not used by the analysis)',
        'outputs': {}, 'runs': {}, 'result_digest': {'checkpoints': {}, 'manager_frames': {}},
        'limitations': [
            'Play research backend, not hardware; vblank numbers and the traversal at which card readiness is observed are '
            'observer-dependent (control: 510->530->540 at calls 1/2/5 minimal vs 1/3/7 hooked), so per-traversal RNG draw '
            'counts before readiness differ between observer configurations; Cancel-to-death (32 traversals), the call count '
            'to the stop, every resource state and VRAM are observer-independent in the control',
            'null renderer: draw-written nibbles are conservative ledger rectangles; no image or audio is captured',
            'the observer keeps one hooked-frame stack across EE threads (the card thread interleaves); attribution uses call '
            'sites, not that stack',
            'probes are block-start breakpoints: a probe never reported is "not observed", not "not reached"'],
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
        meta = json.load(open(os.path.join(p, 'run-meta.json')))
        m['run_time_provenance']['run_meta'][r] = meta
        cb = os.path.join(p, 'memcards-before.json')
        if os.path.exists(cb):
            m['card_setup']['runs'][r] = {'before': json.load(open(cb)), 'after': json.load(open(os.path.join(p, 'memcards-after.json'))),
                                          'before_file': pin(cb), 'after_file': pin(os.path.join(p, 'memcards-after.json'))}
    for root, _, names in os.walk(an):
        for n in sorted(names):
            q = os.path.join(root, n)
            m['outputs']['analysis/' + os.path.relpath(q, an)] = pin(q)
    m['outputs']['analysis.log'] = pin(os.path.join(ev, 'analysis.log'))
    for cp in cmp.CHECKPOINTS:
        c = ref['checkpoints'][cp]
        m['result_digest']['checkpoints'][cp] = {'resource_events': c['resource_events'], 'rng_draws': c['rng_draws_before'],
                                                 'vram_known_nibbles': c['vram']['known_nibbles_required'],
                                                 'vram_unknown_nibbles': c['vram']['unknown_nibbles_required'],
                                                 'vb': c['vb'], 't': c['t']}
    rows = [json.loads(x) for x in open(os.path.join(an, 'manager-frames.jsonl'))]
    m['result_digest']['manager_frames'] = cmp.anchors([{k: r[k] for k in cmp.FRAME_KEYS} for r in rows])
    with open(out, 'x') as f:
        json.dump(m, f, indent=1)
        f.write('\n')
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
