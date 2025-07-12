#!/usr/bin/env python3
"""Write docs/retire-validation.json: hashes of Retire sources, pinned binaries, inputs and retained evidence.

usage: retire_validation_manifest.py OUT.json   (repository root; after tools/retire_validation_run.sh final)
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
V = "local/research/retire-validation-v1"
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

def main(out):
    sources = [
        "include/okage/startup_textures.hpp", "src/startup_textures.cpp", "include/okage/texture_handles.hpp",
        "src/texture_handles.cpp", "include/okage/scene_renderer.hpp", "src/scene_renderer.mm", "src/native_runtime.mm",
        "src/title_session.hpp", "src/title_session.mm", "src/title.cpp", "include/okage/input_log.hpp",
        "src/input_log.cpp", "include/okage/startup_inputs.hpp", "src/startup_inputs.cpp", "CMakeLists.txt",
        "tools/title_regression_align.py", "tools/retire_input_provenance.py", "tools/retire_title_contract.py",
        "tools/retire_title_capture.sh", "tools/retire_title_original_extract.py", "tools/retire_title_compare.py",
        "tools/retire_title_make_manifest.py", "tools/retire_route_compare.py", "tools/retire_route_regression.sh",
        "tools/retire_validation_run.sh", "tools/play-oracle/startup.cpp", "tools/rng_cold_start_regression.sh",
        "tests/startup_textures_tests.cpp", "tests/test_title_regression_align.py", "tests/test_retire_input_provenance.py",
        "tests/test_retire_title_contract.py", "tests/test_retire_title_compare.py", "tests/test_retire_route_compare.py",
        "tests/test_retire_routes.py", "tests/data/retire-routes/restore/buttons.txt",
        "tests/data/retire-routes/newgame/buttons.txt", "tests/data/retire-routes/newgame/directions.txt",
        "tests/data/retire-routes/restore-scripted/title-pad.txt", "tests/data/retire-routes/newgame-scripted/title-pad.txt",
        "docs/retire-title-contract.json", "docs/retire-title-original-manifest.json",
    ]
    evidence = [f"{V}/bin/binaries.sha256", f"{V}/bin/source-head.txt", f"{V}/ctest-release-all.out",
                f"{V}/ctest-debug-focused.out", f"{V}/ctest-sanitize-focused.out", f"{V}/python-suite.out",
                f"{F}/binaries-check.txt", f"{F}/commands.txt", f"{F}/source-head.txt",
                "local/research/retire-alignment-v1/final-1800.out", "local/research/retire-alignment-v1/final2-1800.out",
                "local/research/retire-alignment-v1/synthetic.out", "local/research/retire-alignment-v1/sha256.txt"]
    sessions = {}
    for d in sorted((ROOT / F / "sessions").iterdir()):
        rel = f"{F}/sessions/{d.name}"
        for name in ("command.txt", "stdout.txt", "reads.jsonl", "reads-check.json", "compare.txt", "compare.json",
                     "report/route.json", "report/title-updates.jsonl", "report/native-tails.json",
                     "report/successor-entry/state.json", "report/successor-entry/lifecycle.jsonl"):
            evidence.append(f"{rel}/{name}")
        cmp_txt = text(f"{rel}/compare.txt")
        sessions[d.name] = {"exit": int(text(f"{rel}/exit.txt")), "input_check_exit": int(text(f"{rel}/reads-check.exit")),
                            "original_compare_exit": int(text(f"{rel}/compare.exit")),
                            "result": cmp_txt.splitlines()[-1],
                            "title_updates": next((l.strip() for l in cmp_txt.splitlines() if "title updates:" in l), None),
                            "route": json.loads(text(f"{rel}/report/route.json")),
                            "native_tails": json.loads(text(f"{rel}/report/native-tails.json")),
                            "consumed_assets": len(json.loads(text(f"{rel}/reads-check.json"))["consumed_assets"])}
    gates = {}
    for g in ("release", "sanitize"):
        rel = f"{F}/gates-{g}"
        gates[g] = {k: text(f"{rel}/{k}.txt").splitlines()[-2:] for k in ("rng", "timeline", "startup", "title")}
        evidence += [f"{rel}/{k}.txt" for k in ("rng", "timeline", "startup", "title")] + [f"{rel}/stdout.txt"]
    regressions = {}
    for r in ("restore", "newgame"):
        rel = f"{F}/regression-{r}"
        res = pair_results(f"{rel}/compare-v2.txt")
        regressions[r] = {"pairs": res, "first_pass_compare_without_storage_word_exclusion": pair_results(f"{rel}/compare.txt")}
        evidence += [f"{rel}/compare.txt", f"{rel}/compare-v2.txt", f"{rel}/binaries.sha256", f"{rel}/stage-manifest.sha256",
                     f"{rel}/input/sha256.txt"]
        for run in sorted(p.name for p in (ROOT / rel).iterdir() if re.fullmatch(r"(old|new)-[a-d]", p.name)):
            evidence += [f"{rel}/{run}/command.txt", f"{rel}/{run}/outputs.sha256", f"{rel}/{run}/stdout.txt"]
    cold = f"{F}/regression-cold"
    regressions["cold-1800"] = pair_results(f"{cold}/align.txt")
    evidence += [f"{cold}/align.txt", f"{cold}/binaries.sha256"]
    manifest = {
        "schema": "okage.retire.validation.v1",
        "scope": "Retire: fixes-first acceptance/provenance; the persistent Session texture owner from the first 1296f8 title update through title retirement to entry of 124548 (successor body not run) on Start->Restore and Start->Left->New Game.",
        "base_commit": "962cf6f",
        "binaries_built_from": text(f"{V}/bin/source-head.txt"),
        "head_commit_at_generation": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "elf_sha256": "2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5",
        "stage_manifest_sha256_pinned": "1696227bf62b137522e12ea2f7529a0138f4ffeea449987901a140b6b57d7ad6",
        "run_time_evidence": {
            "sessions": sessions, "gates": gates, "regressions": regressions,
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
        "commands": {"driver": "tools/retire_validation_run.sh final", "per_run": f"{F}/commands.txt",
                     "manifest": "python3 tools/retire_validation_manifest.py docs/retire-validation.json"},
    }
    Path(out).write_text(json.dumps(manifest, indent=1) + "\n")

if __name__ == "__main__":
    main(sys.argv[1])
