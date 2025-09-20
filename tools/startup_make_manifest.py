#!/usr/bin/env python3
"""Write docs/startup-original-manifest.json: pins binary, revisions, commands, inputs and output hashes.

usage: startup_make_manifest.py EVIDENCE_DIR WORKTREE MAIN_CHECKOUT SOURCE_COMMIT OUT.json
Reads files only; never modifies evidence.
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

def tree(root):
    out = {}
    for dp, dn, fn in os.walk(root):
        dn.sort()
        for n in sorted(fn):
            p = os.path.join(dp, n)
            out[os.path.relpath(p, root)] = {'sha256': fsha(p), 'bytes': os.path.getsize(p)}
    return out

def main():
    ev, wt, main_co, commit, out = sys.argv[1:6]
    cache = open(os.path.join(wt, 'build/startup-oracle/CMakeCache.txt')).read()
    def cv(k):
        m = re.search(r'^' + re.escape(k) + r'(?::[A-Z]+)?=(.*)$', cache, re.M)
        return m.group(1) if m else None
    play = os.path.join(main_co, 'references/Play')
    play_rev = open(os.path.join(play, '.git/refs/heads/master')).read().strip()
    disc_report = json.load(open(os.path.join(wt, 'docs/disc-report.json')))
    disc_bin = os.path.join(main_co, 'local/disc/Okage - Shadow King (USA).bin')
    analysis = json.load(open(os.path.join(ev, 'analysis/analysis.json')))
    m = {
        'schema': 'okage.startup-original-manifest.v1',
        'scope': "Independent original-execution reference of the startup texture-resource prefix: unchanged retail SCUS_971.29 run from cold boot under the Play research backend (full VM, disc mounted, research HLE services) and stopped BEFORE EE PC 0x100618.",
        'source_commit': commit,
        'base_commit': '588a94a',
        'play': {'revision': play_rev, 'path': 'references/Play (main checkout, ignored)', 'tree_cleanliness': 'not verified'},
        'oracle': {
            'binary': 'build/startup-oracle/okage-oracle/okage-startup-oracle',
            'binary_sha256': fsha(os.path.join(wt, 'build/startup-oracle/okage-oracle/okage-startup-oracle')),
            'retained_copy': os.path.join('local/research/startup-original-v1/bin/okage-startup-oracle'),
            'retained_copy_sha256': fsha(os.path.join(ev, 'bin/okage-startup-oracle')),
            'build_dir': 'build/startup-oracle (fresh; configured and built from scratch)',
            'configure': 'cmake -S <main>/references/Play -B build/startup-oracle -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_PLAY=OFF '
                         '-DBUILD_TESTS=ON -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 '
                         '-DCMAKE_PROJECT_Play_INCLUDE=<worktree>/tools/play-oracle/Attach.cmake -DDEBUGGER_INCLUDED=ON -DOKAGE_REFERENCE_VULKAN=OFF',
            'build': 'ninja -C build/startup-oracle okage-startup-oracle',
            'cmake_build_type': cv('CMAKE_BUILD_TYPE'), 'cxx_compiler': cv('CMAKE_CXX_COMPILER'),
            'osx_architectures': cv('CMAKE_OSX_ARCHITECTURES'), 'osx_sysroot': cv('CMAKE_OSX_SYSROOT'),
            'cxx_version': 'Apple clang version 17.0.0 (clang-1700.3.19.1), target arm64-apple-darwin25.0.0',
        },
        'tracked_sources_sha256': {p: fsha(os.path.join(wt, p)) for p in (
            'tools/play-oracle/startup.cpp', 'tools/play-oracle/CMakeLists.txt', 'tools/play-oracle/Attach.cmake',
            'tools/startup_capture.sh', 'tools/startup_original_report.py', 'tools/startup_make_manifest.py',
            'tests/test_startup_original.py')},
        'inputs': {
            'elf': {'path': 'local/extracted/SCUS_971.29', 'sha256': fsha(os.path.join(main_co, 'local/extracted/SCUS_971.29'))},
            'disc_cue': {'path': 'local/disc/Okage - Shadow King (USA).cue', 'sha256': fsha(os.path.join(main_co, 'local/disc/Okage - Shadow King (USA).cue'))},
            'disc_bin': {'path': 'local/disc/Okage - Shadow King (USA).bin', 'sha256': fsha(disc_bin), 'bytes': os.path.getsize(disc_bin),
                         'docs_disc_report_sha256': disc_report['image_sha256']},
            'contract_json': {'path': 'docs/startup-contract.json', 'sha256': fsha(os.path.join(wt, 'docs/startup-contract.json'))},
            'play_config': 'fresh default configuration created in <cwd>/local/play-boot-config (no saved state, no pad input, no sound handler)',
        },
        'fixed_settings': {
            'pad': 'RecordedPad with no input rows (neutral sticks)', 'sound_handler': 'none', 'gs': 'null renderer sink with host-to-local writes + ledger',
            'stop': 'EE PC 0x100618 (jal 0x114f50), reached as a block start; the instruction has not executed',
            'vram_initial_fill': 'applied before the first guest instruction (EE PC 0x100008); run pairs use 0x00 and 0xff',
            'deadline_seconds': 1500,
        },
        'commands': [
            'tools/startup_capture.sh <oracle> "<main>/local/disc/Okage - Shadow King (USA).cue" <main>/local/extracted/SCUS_971.29 <main>/local/research/startup-original-v1/runs',
            'python3 -I tools/startup_original_report.py <evidence>/runs --contract docs/startup-contract.json --out <evidence>/analysis',
            'python3 -I tools/startup_make_manifest.py <evidence> <worktree> <main> <source_commit> docs/startup-original-manifest.json',
            'python3 -m unittest discover -s tests -v',
        ],
        'evidence_dir': 'local/research/startup-original-v1 (read-only after capture)',
        'outputs': tree(ev),
        'result_digest': {
            'byte_identical_trace_runs': analysis['determinism']['byte_identical_trace_runs'],
            'ee_ram_identical_across_fill_runs': analysis['determinism']['ee_ram_identical_across_fill_runs'],
            'top_level_call_sequence_identical_to_contract': analysis['top_level_calls']['identical_sequence'],
            'contract_field_differences': len(analysis['contract_comparison']['field_differences']),
            'contract_global_order_differences': analysis['contract_comparison']['global_order_difference_count'],
            'written_nibbles': analysis['vram_written_mask']['written_nibbles'],
            'written_stream_sha256': analysis['vram_written_mask']['written_stream_sha256'],
            'gs_vertex_kicks': analysis['gs_cross_check']['vram_outside_uploads']['gs_vertex_kicks'],
            'rand_calls_total': analysis['claims']['rand']['total_rand_calls_observed'],
        },
        'limitations': [
            'Play research backend, Debug build; not a physical PS2 proof. Play tree cleanliness not verified.',
            'Breakpoints are block-start breakpoints (Play compiles the check at the start of any basic block containing a breakpoint address): function entries and jal+8 return sites are exact; top-level call-site markers are emitted at the containing block start before the jal executes.',
            'Observed run reaches the stop after 84 vblanks, the unhooked control run after 75; allocator/handle/descriptor/VRAM state is identical, EE RAM differs in 9 4 KiB pages (timing/stack).',
            'Written-lane mask comes from complementary-fill runs of the null-renderer GS, not from a log of local-memory writes. Draws, local-to-local copies and CLUT loads are counted (none observed) but not rendered; privileged-register writes are not ledgered.',
            'DMA channel CHCR read-back at the stop is the raw register value and is not used as evidence; all 72 host-to-local transfers completed in the GS ledger.',
            'IOP-level disc opens (ioman) only show module loads; CMNDATA/FONT reads go through the CDVD RPC path and are observed at the EE wrapper 0x1c2e68/0x1c29d8.',
            'ee-ram-32mb.bin is the first 32 MiB of EE RAM only.',
        ],
    }
    json.dump(m, open(out, 'w'), indent=1)
    print('wrote', out)

if __name__ == '__main__':
    main()
