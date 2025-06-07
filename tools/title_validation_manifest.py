#!/usr/bin/env python3
"""Write docs/title-lifecycle-validation.json: hashes of RNG sources, binaries, imported inputs and evidence.

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
    V = "local/research/title-lifecycle-validation-v1"
    S = "local/research/title-session-native-v1"
    R = "local/research/title-regression-v1/final2-1800"
    sources = [
        "src/effect_geometry.cpp", "tests/effect_tests.cpp", "include/okage/startup_textures.hpp", "src/startup_textures.cpp",
        "include/okage/texture_handles.hpp", "src/texture_handles.cpp", "include/okage/texture_memory.hpp",
        "src/texture_memory.cpp", "include/okage/texture_transfer.hpp", "src/texture_transfer.cpp", "src/assets.cpp",
        "src/native_runtime.mm", "src/title_session.hpp", "src/title_session.mm", "src/scene_renderer.mm",
        "include/okage/scene_renderer.hpp", "CMakeLists.txt", "tools/title_compare.py",
        "tools/title_original_extract.py", "tools/title_regression_align.py", "tools/title_contract.py",
        "tests/startup_textures_tests.cpp", "tests/texture_handles_tests.cpp", "tests/texture_psm1_tests.cpp",
        "tests/test_title_compare.py", "tests/test_title_again.py", "tests/test_title_regression_align.py",
        "docs/title-original-manifest.json", "docs/title-psm1-validation.json", "docs/title-contract.json",
    ]
    evidence = [
        "local/research/title-shape-guard-v1/invalid-shape.out",
        f"{V}/bin/binaries.sha256", f"{V}/bin/source-head.txt", f"{V}/ctest-focused.out", f"{V}/ctest-release-all.out",
        f"{V}/python-suite.out", f"{V}/release-lifecycle.out", f"{V}/title-again-input/buttons.txt",
        f"{V}/title-again-input/bkmo0.dat", f"{R}/binaries.sha256", f"{R}/align.txt",
    ]
    if (ROOT / V / "asan-lifecycle.out").is_file():
        evidence.append(f"{V}/asan-lifecycle.out")
    for v in ("release", "sanitize"):
        for f in ("command.txt", "stdout.txt", "rng-compare.out", "timeline-compare.out", "state-compare.out",
                  "title-compare.out", "title-report/state.json", "title-report/timeline.jsonl",
                  "title-report/vram.bytes", "title-report/vram.known", "startup-report/state.json"):
            evidence.append(f"{S}/final-{v}/{f}")
    for tag in ("old-a", "old-b", "new-a", "new-b"):
        evidence += [f"{R}/{tag}/command.txt", f"{R}/{tag}/outputs.sha256", f"{R}/{tag}/stdout.txt"]
    manifest = {
        "schema": "okage.title.validation.v1",
        "scope": "Title: shape-guard fix; persistent Session texture owner through storage, logos, logo retirement and title construction up to the cold-start 12c350(selector=1) return, before the first 1296f8 update.",
        "base_commit": "7a93d87",
        "head_commit_at_generation": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "elf_sha256": "2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5",
        "tracked_sources": {p: sha(ROOT / p) for p in sources},
        "evidence": [entry(p) for p in evidence],
        "commands": {
            "session": (ROOT / S / "final-release/command.txt").read_text().strip(),
            "compare_title": f"python3 tools/title_compare.py local/research/title-original-v1/analysis {S}/final-<v>/title-report",
            "compare_rng": "tools/rng_compare.py, tools/rng_startup_timeline_compare.py, tools/startup_compare.py on <run>/startup-report",
            "regression": f"tools/rng_cold_start_regression.sh {V}/bin/old-rng.app/... {V}/bin/new-release.app/... local/game-data-stage-v13 {R} 1800",
            "align": "python3 tools/title_regression_align.py RUN_A RUN_B (every pair; see align.txt)",
            "focused_ctest": "build/rng-{release,debug,sanitize}: startup-inputs, rng-cold-start-inputs, title-again, effect, field, startup-textures, texture-(transfer|psm1|snapshot|memory|handles|format), xim-raw, asset, gs-texture-address",
            "python": "OKAGE_MAIN_CHECKOUT=$PWD OKAGE_NATIVE=... OKAGE_HANDLE_TRACE=... OKAGE_TRANSFER_COMPARE=... python3 -m unittest discover -s tests",
        },
        "results": {
            "title_compare_release": lines(f"{S}/final-release/title-compare.out", 18),
            "title_compare_sanitize": lines(f"{S}/final-sanitize/title-compare.out", 2),
            "rng_preserved": [lines(f"{S}/final-{v}/{f}", 1)[0] for v in ("release", "sanitize")
                              for f in ("rng-compare.out", "timeline-compare.out", "state-compare.out")],
            "focused_ctest": lines(f"{V}/ctest-focused.out"),
            "full_release_ctest": lines(f"{V}/ctest-release-all.out"),
            "python_suite": lines(f"{V}/python-suite.out"),
            "release_lifecycle_handle": lines(f"{V}/release-lifecycle.out"),
            "asan_lifecycle_handle": lines(f"{V}/asan-lifecycle.out", 3) if (ROOT / V / "asan-lifecycle.out").is_file() else "pending",
            "regression_alignment": lines(f"{R}/align.txt"),
        },
        "review_findings": [
            "HIGH second title construction threw (fixed 85515df; runtime replay test title-again)",
            "regression alignment could pass truncated runs (fixed: counts, explicit record lengths, logo phase, controls)",
            "regression binaries not pinned (fixed: copies in title-lifecycle-validation-v1/bin, rerun final2-1800)",
            "sanitize session comparisons not retained (fixed: final-sanitize/*-compare.out)",
            "PSM1 comment/doc stale (fixed); NIGHT_V a.xmd now passed through (XIM host decode still path-based)",
        ],
        "unresolved": [
            "game over -> title has no runtime replay (same guarded branch as Restore -> title)",
            "NIGHT_V XIM host decode still reads the archive path (logical upload uses the 186aa8 bytes)",
            "PSM1 final partial pixel follows Play zero-extension (unverified on hardware; unreached)",
            "draw invalidation is a conservative owner-lane superset, not a raster; A03 glyph cache unknown",
            "Play research backend, not hardware",
        ],
    }
    Path(out).write_text(json.dumps(manifest, indent=1) + "\n")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1])
