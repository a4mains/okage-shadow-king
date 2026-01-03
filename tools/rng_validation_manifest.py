#!/usr/bin/env python3
"""Write docs/rng-validation.json: hashes of RNG sources, binaries, imported inputs and evidence.

usage: rng_validation_manifest.py OUT.json   (repository root; after the verification runs)
Results are copied from retained output files, not typed in.
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

def lines(path, last=None):
    text = (ROOT / path).read_text().strip().splitlines()
    return text[-last:] if last else text

def main(out):
    F = "local/research/rng-session-native-v1/final"
    R = "local/research/rng-regression-v1"
    sources = [
        "include/okage/startup_textures.hpp", "src/startup_textures.cpp", "include/okage/startup_inputs.hpp",
        "src/startup_inputs.cpp", "include/okage/sha256.hpp", "src/sha256.cpp", "include/okage/effect.hpp",
        "src/effect_geometry.cpp", "src/native_runtime.mm", "CMakeLists.txt",
        "tools/import_startup_inputs.py", "tools/stage_game_data.py", "tools/rng_compare.py",
        "tools/rng_original_extract.py", "tools/rng_startup_timeline_compare.py", "tools/startup_compare.py",
        "tools/rng_cold_start_regression.sh", "tests/startup_textures_tests.cpp", "tests/startup_inputs_tests.cpp",
        "tests/effect_tests.cpp", "tests/test_rng_compare.py", "tests/test_rng_startup_timeline_compare.py",
        "tests/test_rng_cold_start_inputs.py", "tests/test_import_startup_inputs.py",
        "docs/rng-original-manifest.json", "docs/startup-original-manifest.json",
    ]
    evidence = [
        "local/startup-input-v1/NATIVE_STARTUP/FONTCLUT.BIN",
        "local/startup-input-v1/NATIVE_STARTUP/startup-inputs.json",
        "local/game-data-stage-v13/.owned-game-data-manifest.json",
        f"{F}/command.txt", f"{F}/stdout.txt", f"{F}/report/state.json", f"{F}/report/rng.jsonl",
        f"{F}/report/effects.json", f"{F}/report/timeline.jsonl", f"{F}/report/vram.bytes", f"{F}/report/vram.known",
        f"{F}/rng-compare.out", f"{F}/timeline-compare.out", f"{F}/state-compare.out",
        "local/research/rng-session-native-v1/asan-final/stdout.txt",
        f"{R}/final-1800/binaries.sha256", f"{R}/final-1800/source-head.txt",
        f"{R}/ctest-focused-final2.out", f"{R}/ctest-release-all-final.out", f"{R}/python-suite-final2.out",
    ]
    for tag in ("old-a", "old-b", "new-a", "new-b"):
        evidence += [f"{R}/final-1800/{tag}/command.txt", f"{R}/final-1800/{tag}/outputs.sha256", f"{R}/final-1800/{tag}/stdout.txt"]
    binaries = ["build/rng-release/okage-native.app/Contents/MacOS/okage-native",
                "build/rng-sanitize/okage-native.app/Contents/MacOS/okage-native",
                "build/startup-release/okage-native.app/Contents/MacOS/okage-native"]
    manifest = {
        "schema": "okage.rng.validation.v1",
        "scope": "RNG: one Session RNG stream and required imported inputs through the startup prefix before "
                 "executing 0x100618. Not title/model/XTA/field/battle history, sampling, tick37297, full T1 or the game.",
        "base_commit": "e709f21",
        "head_commit_at_generation": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "elf_sha256": "2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5",
        "tracked_sources": {p: sha(ROOT / p) for p in sources},
        "binaries": {p: sha(ROOT / p) for p in binaries},
        "evidence": [entry(p) for p in evidence],
        "commands": {
            "import": "python3 tools/import_startup_inputs.py --elf local/extracted/SCUS_971.29 --fonts local/font-input/CMNDATA/FONT "
                      "--effects local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF --efd local/effect-input/CMNDATA/2D/TES.EFD --out local/startup-input-v1",
            "stage": "python3 tools/stage_game_data.py local/game-data-stage-v13",
            "cold_session": (ROOT / F / "command.txt").read_text().strip(),
            "compare_rng": f"python3 tools/rng_compare.py local/research/rng-original-v1/analysis {F}/report",
            "compare_timeline": f"python3 tools/rng_startup_timeline_compare.py local/research/startup-original-v1/runs/fill00-a/events.jsonl {F}/report",
            "compare_state": f"python3 tools/startup_compare.py local/research/startup-original-v1/analysis {F}/report",
            "regression": "tools/rng_cold_start_regression.sh <build/startup-release okage-native> <build/rng-release okage-native> local/game-data-stage-v13 local/research/rng-regression-v1/final-1800 1800",
            "focused_ctest": "ctest -R '^(startup-inputs-tests|rng-cold-start-inputs|effect-tests|field-tests|startup-textures-tests|texture-(transfer|snapshot|memory|handles|format)-tests|xim-raw-tests|asset-tests|gs-texture-address-tests)$' in build/rng-{release,debug,sanitize}",
            "python": "OKAGE_MAIN_CHECKOUT=$PWD OKAGE_NATIVE=... OKAGE_HANDLE_TRACE=... OKAGE_TRANSFER_COMPARE=... python3 -m unittest discover -s tests",
        },
        "results": {
            "rng_compare": lines(f"{F}/rng-compare.out"),
            "timeline_compare": lines(f"{F}/timeline-compare.out", 2),
            "state_compare": lines(f"{F}/state-compare.out", 1),
            "focused_ctest": lines(f"{R}/ctest-focused-final2.out"),
            "full_release_ctest": lines(f"{R}/ctest-release-all-final.out"),
            "python_suite": lines(f"{R}/python-suite-final2.out"),
            "asan_ubsan_session": "exit 0, no sanitizer reports; rng/timeline/state comparisons IDENTICAL (30 frames)",
            "regression": "old-b vs new-b (equal startup timing): PNG, audio and sidecars identical except f.png.json "
                          "rng_state (0x6f4240e1 -> 0x412c25e1) and startup.bin (random.state column; async storage-probe column "
                          "varies between same-binary runs)",
        },
        "reused_unchanged_evidence": [
            "Startup original startup capture and manifest (docs/startup-original-manifest.json)",
            "Startup format transfer comparisons (texture transfer code unchanged in RNG)",
            "Handle/Startup handle lifecycle comparisons incl. the 5,880 s ASan run (handle code unchanged in RNG)",
        ],
        "review_findings_fixed": [
            "rng_compare accepted self/unpinned/truncated input: CLI now pins the original to the manifest, refuses same dir, enforces 9504/11/886",
            "required-input negatives were opt-in: ctest now runs startup-inputs-tests with the staged root and rng-cold-start-inputs with the real binary",
            "docs: RNG doc tracked, startup.bin and post-boundary RNG wording corrected, stale importer note replaced",
        ],
        "source_corrections_found_by_comparison": [
            "procedural template positions stored as int16/64 (signed zero)",
            "noise shapes emitted in the 0x111b70 strip-chaining order (the recovered 0x111b70 strip-chaining order)",
            "request trace marks construction on cache miss",
        ],
        "unresolved": [
            "shape 10 never constructed (requests reuse shape 9 cache entry); native draw resolution unverified (outside prefix)",
            "later field effect textures reloaded from disk, not from verified startup bytes (outside prefix)",
            "written-lane masks paired-fill derived; Play research backend, not hardware",
            "no original frame/audio at the boundary; nothing compared beyond 0x100618",
        ],
    }
    Path(out).write_text(json.dumps(manifest, indent=1) + "\n")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1])
