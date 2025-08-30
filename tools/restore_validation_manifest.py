#!/usr/bin/env python3
"""Write docs/restore-ownership-validation.json: hashes of Retire sources, pinned binaries, inputs and retained evidence.

usage: restore_validation_manifest.py OUT.json   (repository root; after tools/restore_validation_run.sh final)
Results are read from retained output files, not typed in. Current-time audits (re-hashing what exists
now) are labelled separately from run-time evidence recorded when the runs happened.
"""
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path.cwd()
V = "local/research/restore-ownership-validation-v3"
F = f"{V}/final"

def sha(path):
    h = hashlib.sha256()
    with open(ROOT / path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def entry(path):
    return {"path": path, "sha256": sha(path), "bytes": (ROOT / path).stat().st_size}

def text(path):
    return (ROOT / path).read_text().strip()

def pair_results(path):
    out, cur = {}, None
    for line in text(path).splitlines():
        if line.startswith("== "):
            cur = line[3:]
        elif line.startswith("result: "):
            out[cur] = line[8:]
    return out

SCOPE = ("Restore: fixes-first route-comparator coverage/causality (F1/F2); the persistent Session texture owner from entry of "
         "124548 (mode 10) through empty-card Restore, Cancel, manager/window/resource/wrapper retirement to the return of "
         "the second 12c350(selector=1). The scripted native Sessions compare IDENTICAL (strict) with the light original "
         "reference at eight checkpoints: 1,800 observed lifecycle rows, 0 owner-asserted, all 13 state fields, effect106, "
         "accounted VRAM, 13,868 Session RNG rows and 51 manager frames. RNG provenance: 9,504 prefix draws are hooked "
         "observations; the 4,364 Restore-window draws are reconstructed by the recovered LCG between 109 observed RNG-state "
         "anchors (not individually hooked, call sites unobserved); manager rows combine observed input/state/pad words "
         "with derived per-traversal draw counts. Not image/audio parity, saved slots, loading, name entry, "
         "field/battle/XTA, sampling, tick37297, the full texture port or the game.")
DRIVER = "RESTORE_VALIDATION=local/research/restore-ownership-validation-v3 tools/restore_validation_run.sh final"

def main(out):
    sources = [
        "include/okage/startup_textures.hpp", "src/startup_textures.cpp", "include/okage/texture_handles.hpp",
        "src/texture_handles.cpp", "src/native_runtime.mm", "src/restore_session.hpp", "src/restore_session.mm",
        "src/ui_window_glyph.cpp", "include/okage/rng_site.hpp", "include/okage/enemy.hpp", "include/okage/effect.hpp",
        "src/effect.cpp", "include/okage/startup_inputs.hpp", "src/startup_inputs.cpp", "CMakeLists.txt",
        "tools/retire_route_compare.py", "tools/retire_route_regression.sh", "tools/restore_validation_run.sh",
        "tools/restore_contract.py", "tools/restore_capture.sh", "tools/restore_original_extract.py",
        "tools/restore_compare.py", "tools/restore_make_manifest.py", "tools/play-oracle/startup.cpp",
        "tools/retire_input_provenance.py", "tools/restore_io_timing_evidence.py", "tests/test_restore_io_timing_evidence.py",
        "tests/startup_textures_tests.cpp", "tests/test_retire_route_compare.py",
        "tests/test_restore_contract.py", "tests/test_restore_compare.py", "tests/test_restore_routes.py",
        "tests/data/restore-routes/restore-cancel/buttons.txt", "tests/data/restore-routes/pending-cancel/buttons.txt",
        "docs/restore-contract.json", "docs/restore-original-manifest.json",
    ]
    evidence = [f"{V}/bin/binaries.sha256", f"{V}/bin/source-head.txt", f"{V}/ctest-release-all.out",
                f"{V}/ctest-debug-focused.out", f"{V}/ctest-sanitize-focused.out", f"{V}/python-suite.out",
                f"{F}/binaries-check.txt", f"{F}/commands.txt", f"{F}/source-head.txt",
                "local/research/restore-route-recheck-v1/regression-restore.out",
                "local/research/restore-route-recheck-v1/regression-newgame.out",
                "local/research/restore-route-recheck-v1/sha256.txt",
                "local/research/restore-ownership-validation-v1/bin/binaries.sha256",
                "local/research/restore-ownership-validation-v2/bin/binaries.sha256",
                "docs/restore-light-manifest.json",
                "local/research/restore-ownership-validation-v1/conditional-parity/sha256.txt",
                "local/research/restore-ownership-validation-v1/io-timing/sha256.txt"]
    sessions = {}
    for d in sorted((ROOT / F / "sessions").iterdir()):
        rel = f"{F}/sessions/{d.name}"
        names = ["command.txt", "stdout.txt", "reads.jsonl", "reads-check.json", "saves-before.txt", "saves-after.txt",
                 "report/route.json", "report/manager-frames.jsonl", "report/session-draw-frames.jsonl",
                 "report/second-title-return/state.json", "report/second-title-return/lifecycle.jsonl",
                 "report/second-title-return/rng.jsonl"]
        has_cmp = (ROOT / rel / "compare.exit").exists()
        if has_cmp:
            names += ["compare.txt", "compare.json"]
        has_light = (ROOT / rel / "light-compare.exit").exists()
        if has_light:
            names += ["light-compare.txt", "light-compare.json"]
        has_io = (ROOT / rel / "io-timing.exit").exists()
        if has_io:
            names += ["io-timing.json", "io-timing.txt"]
        evidence += [f"{rel}/{n}" for n in names if (ROOT / rel / n).exists()]
        entry_ = {"exit": int(text(f"{rel}/exit.txt")), "input_check_exit": int(text(f"{rel}/reads-check.exit")),
                  "saves_empty_before_after": [text(f"{rel}/saves-before.txt"), text(f"{rel}/saves-after.txt")],
                  "route": json.loads(text(f"{rel}/report/route.json"))}
        if has_cmp:
            c = json.loads(text(f"{rel}/compare.json"))
            entry_.update({"original_compare_exit": int(text(f"{rel}/compare.exit")), "result": c["result"],
                           "checkpoints_lifecycle_state_effect_vram_identical": all(
                               v["events_identical"] and not v["state_differences"] and v["effect106_identical"]
                               and not v["stream_inconsistent"] for v in c["checkpoints"].values()),
                           "timing_diagnostic": c.get("timing_diagnostic"), "asserted": c.get("asserted"),
                           "anchors_original": c.get("anchors_original"), "anchors_native": c.get("anchors_native"),
                           "readiness_original": c.get("readiness_original"), "readiness_native": c.get("readiness_native"),
                           "conditional_parity": c.get("conditional_parity")})
        if has_light:
            lc = json.loads(text(f"{rel}/light-compare.json"))
            entry_.update({"light_reference_compare_exit": int(text(f"{rel}/light-compare.exit")),
                           "light_reference_result": lc.get("result"), "light_reference_asserted": lc.get("asserted")})
        if has_io:
            io_ = json.loads(text(f"{rel}/io-timing.json"))
            entry_.update({"exact_parity_exit": int(text(f"{rel}/io-timing.exit")),
                           "native_matches_an_original_execution": io_["native_matches_an_original_execution"],
                           "native_exact_rng_parity": io_["native_exact_rng_parity"],
                           "native_known_vram_vs_control": io_["native_known_vram_vs_control"],
                           "draw_counts": {k: io_[k] for k in ("original-hooked", "original-control", "native")}})
        sessions[d.name] = entry_
    retire = {}
    for r in ("restore", "newgame"):
        rel = f"{F}/retire-{r}"
        retire[r] = {"exit": int(text(f"{rel}/exit.txt")), "strict_compare_exit": int(text(f"{rel}/compare.exit")),
                  "result": text(f"{rel}/compare.txt").splitlines()[-1]}
        evidence += [f"{rel}/compare.txt", f"{rel}/stdout.txt"]
    gates = {}
    for g in ("release", "sanitize"):
        rel = f"{F}/gates-{g}"
        gates[g] = {k: text(f"{rel}/{k}.txt").splitlines()[-2:] for k in ("rng", "timeline", "startup", "title")}
        evidence += [f"{rel}/{k}.txt" for k in ("rng", "timeline", "startup", "title")] + [f"{rel}/stdout.txt"]
    regressions = {}
    rel = f"{F}/regression-restore-cancel"
    regressions["restore-cancel"] = pair_results(f"{rel}/compare.txt")
    evidence += [f"{rel}/compare.txt", f"{rel}/binaries.sha256", f"{rel}/stage-manifest.sha256", f"{rel}/input/sha256.txt"]
    for run in sorted(p.name for p in (ROOT / rel).iterdir() if re.fullmatch(r"(old|new)-[a-d]", p.name)):
        evidence += [f"{rel}/{run}/command.txt", f"{rel}/{run}/outputs.sha256", f"{rel}/{run}/stdout.txt"]
    cold = f"{F}/regression-cold"
    regressions["cold-1800"] = pair_results(f"{cold}/align.txt")
    evidence += [f"{cold}/align.txt", f"{cold}/binaries.sha256"]
    manifest = {
        "schema": "okage.restore.validation.v1",
        "scope": SCOPE,
        "base_commit": "82ec28a",
        "binaries_built_from": text(f"{V}/bin/source-head.txt"),
        "head_commit_at_generation": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "elf_sha256": "2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5",
        "stage_manifest_sha256_pinned": "1696227bf62b137522e12ea2f7529a0138f4ffeea449987901a140b6b57d7ad6",
        "run_time_evidence": {
            "sessions": sessions, "retire_routes_strict": retire, "gates": gates, "regressions": regressions,
            "ctest": {k: text(f"{V}/ctest-{k}.out").splitlines()[-3] for k in ("release-all", "debug-focused", "sanitize-focused")},
            "python_suite": [l for l in text(f"{V}/python-suite.out").splitlines() if l.startswith(("Ran ", "OK", "FAILED"))],
        },
        "current_time_audit": {
            "stage": {k: v for k, v in json.loads(text(f"{F}/stage-audit-current-time.json")).items()
                      if isinstance(v, (int, float, str, bool))},
            "note": "re-hash of the stage when the evidence driver ran; it does not prove historical reads (see sessions.*.input_check_exit for run-time reads)",
        },
        "tracked_sources": {p: sha(p) for p in sources},
        "evidence": [entry(p) for p in evidence],
        "commands": {"driver": DRIVER, "per_run": f"{F}/commands.txt",
                     "manifest": "python3 tools/restore_validation_manifest.py docs/restore-ownership-validation.json"},
    }
    Path(out).write_text(json.dumps(manifest, indent=1) + "\n")

if __name__ == "__main__":
    main(sys.argv[1])
