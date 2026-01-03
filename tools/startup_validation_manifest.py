#!/usr/bin/env python3
"""Write docs/startup-validation.json: hashes of the tracked sources, binaries and retained evidence of Startup.

usage: startup_validation_manifest.py OUT.json   (run from the repository root after the verification runs)
Results are recorded from the retained output files, not typed in.
"""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path.cwd()

def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def entry(path):
    p = ROOT / path
    return {"path": path, "sha256": sha(p), "bytes": p.stat().st_size}

def text(path):
    return (ROOT / path).read_text().strip()

def main(out):
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    base = "a4d807d"
    sources = [
        "include/okage/startup_textures.hpp", "src/startup_textures.cpp", "include/okage/texture_handles.hpp",
        "src/texture_handles.cpp", "include/okage/texture_memory.hpp", "src/texture_memory.cpp",
        "include/okage/texture_transfer.hpp", "src/texture_transfer.cpp", "src/assets.cpp", "src/native_runtime.mm",
        "tools/startup_compare.py", "tools/texture_handle_trace_diff.py", "tools/texture_transfer_compare.cpp",
        "tests/startup_textures_tests.cpp", "tests/test_startup_compare.py", "tests/test_texture_handle_trace.py",
        "tests/texture_handles_tests.cpp", "tests/texture_format_tests.cpp", "docs/startup-contract.json",
        "docs/startup-original-manifest.json", "docs/startup-format-validation.json",
    ]
    evidence = [
        "local/research/startup-original-v1/analysis/final-state.jsonl",
        "local/research/startup-original-v1/analysis/vram-written-mask.bin",
        "local/research/startup-original-v1/analysis/vram-written-values.bin",
        "local/research/startup-session-native-v1/final/report/state.json",
        "local/research/startup-session-native-v1/final/report/vram.bytes",
        "local/research/startup-session-native-v1/final/report/vram.known",
        "local/research/startup-session-native-v1/final/compare.out",
        "local/research/startup-session-native-v1/final/frame.png",
        "local/research/startup-session-native-v1/final/oldrerun/o1.png",
        "local/research/startup-validation-v1/ctest-final.out",
        "local/research/startup-validation-v1/python-suite.out",
        "local/research/startup-validation-v1/release-lifecycle.out",
        "local/research/startup-xim-inventory-v1/inventory.json",
    ]
    asan = ROOT / "local/research/startup-validation-v1/asan-lifecycle.out"
    if asan.exists():
        evidence.append("local/research/startup-validation-v1/asan-lifecycle.out")
    binaries = [
        "build/startup-release/okage-native.app/Contents/MacOS/okage-native",
        "build/native-release/okage-native.app/Contents/MacOS/okage-native",
        "build/startup-release/okage-texture-handle-trace", "build/startup-sanitize/okage-texture-handle-trace",
        "build/startup-release/okage-texture-transfer-compare",
    ]
    manifest = {
        "schema": "okage.startup.validation.v1",
        "scope": "Startup: Session-owned startup texture lifetime through effects and fonts, pre-scheduler only. "
                 "Not full T1, not tick37297, not sampling, not the game.",
        "base_commit": base, "head_commit_at_generation": head,
        "elf_sha256": "2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5",
        "tracked_sources": {p: sha(ROOT / p) for p in sources},
        "binaries": {p: sha(ROOT / p) for p in binaries},
        "evidence": [entry(p) for p in evidence],
        "commands": [
            "cmake -S . -B build/startup-{release,debug} -G Ninja -DCMAKE_BUILD_TYPE={Release,Debug}; "
            "build/startup-sanitize: Debug with -fsanitize=address,undefined -fno-omit-frame-pointer",
            "ctest -R '^(startup-textures-tests|texture-(transfer|snapshot|memory|handles|format)-tests|xim-raw-tests|asset-tests|gs-texture-address-tests)$' in each build (9 tests)",
            "okage-native --game-data local/game-data-stage-v12-moved --save-directory <fresh> --startup-texture-report <fresh dir> --frames 600 --capture <fresh png>",
            "python3 tools/startup_compare.py local/research/startup-original-v1/analysis local/research/startup-session-native-v1/final/report",
            "OKAGE_HANDLE_TRACE=build/startup-{release,sanitize}/okage-texture-handle-trace python3 -m unittest tests.test_texture_handle_lifecycle",
            "OKAGE_HANDLE_TRACE=... OKAGE_TRANSFER_COMPARE=... python3 -m unittest discover -s tests",
        ],
        "results": {
            "focused_ctest": text("local/research/startup-validation-v1/ctest-final.out").splitlines(),
            "python_suite": text("local/research/startup-validation-v1/python-suite.out").splitlines(),
            "release_lifecycle": text("local/research/startup-validation-v1/release-lifecycle.out").splitlines()[-3:],
            "asan_lifecycle": text("local/research/startup-validation-v1/asan-lifecycle.out").splitlines()[-4:] if asan.exists() else "not finished at generation",
            "startup_state_compare": text("local/research/startup-session-native-v1/final/compare.out").splitlines()[-4:],
        },
        "retained_failures_and_findings": [
            "review: startup_compare accepted empty/hand-made input (fixed: manifest pins, exact counts, strict keys); "
            "--accept accepted a trace compared with itself (fixed: same-file refusal, cross-field checks)",
            "review: ELF-missing path skipped the owner silently (fixed: hard error with --startup-texture-report; documented fallback)",
            "my null-CLUT test first over-read 8 blocks into the neighbour CLUT (test bug, fixed: 4 blocks)",
            "pre-change binary run-to-run: startup frame 279 vs 280 (wall-clock loading windows) in both old and new binaries",
        ],
        "unresolved": [
            "post-grid rand draws (160 in 10bc78) and effect-template grid not modeled; Session::random starts at 1",
            "written-lane masks are paired-fill derived, not a GS write log; Play research backend only",
            "input identity (ELF/FNT/XIM hashes) not checked at run time",
            "no original frame/audio exists at the boundary; no native-vs-original image/audio comparison",
            "free-queue node order and recorded display lists not compared",
            "PSM1/27/CPSM2, XTA, title/model/field/battle history, snapshots in prepared frames, sampling: later milestones",
        ],
    }
    Path(out).write_text(json.dumps(manifest, indent=1) + "\n")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1])
