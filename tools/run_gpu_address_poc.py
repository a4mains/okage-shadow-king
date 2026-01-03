#!/usr/bin/env python3
"""Serial G0 runner and strict raw-sample summarizer. No game/window control."""
import argparse
import hashlib
import json
import math
import os
import re
import statistics
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / "docs/gpu-address-protocol-v1.json"
CORPUS = ROOT / "local/research/material-texture-ownership-v1/address-corpus-v1"
PINNED = {"addresses.bin": "db66ea293331e34f852690f8fcf74443141366206db75833537e3be2d6fe8b0d",
          "play-addresses-v1.bin": "6c6c7e468d988ca9d5abb27897d965eb49761d1edcd5a8de2ec8585b2e2c124e"}

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def capture(command):
    return subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, check=True).stdout.strip()

def run(command, destination):
    if destination.exists():
        raise RuntimeError(f"refusing to overwrite {destination}")
    with (destination.parent / "commands.jsonl").open("a") as log:
        log.write(json.dumps({"command": list(map(str, command)), "stdout": str(destination.relative_to(ROOT)),
                              "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}) + "\n")
    with destination.open("w") as out, destination.with_suffix(destination.suffix + ".stderr").open("w") as err:
        result = subprocess.run(command, cwd=ROOT, stdout=out, stderr=err)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}), retained {destination}")

def percentile95(values):
    return sorted(values)[math.ceil(len(values) * .95) - 1]

def observe():
    hid = capture(["ioreg", "-c", "IOHIDSystem"])
    match = re.search(r'"HIDIdleTime" = (\d+)', hid)
    return {"processes": capture(["ps", "-axo", "pid,pcpu,etime,comm"]),
            "thermal": capture(["pmset", "-g", "therm"]),
            "hid_idle_seconds": int(match.group(1)) / 1e9 if match else None,
            "monotonic": time.monotonic()}

def load_reasons(observation):
    reasons = []
    for line in observation["processes"].splitlines()[1:]:
        fields = line.split(None, 3)
        if len(fields) == 4 and int(fields[0]) != os.getpid() and float(fields[1]) >= 50:
            reasons.append("observed process >=50% CPU: " + line.strip())
        if len(fields) == 4 and re.search(r"/(clang[^/]*|cmake|ninja|make|qemu[^/]*|sample|instruments)$", fields[3]):
            reasons.append("build/VM/profiler present: " + line.strip())
    if "No thermal warning level has been recorded" not in observation["thermal"]:
        reasons.append("thermal warning state requires review")
    return reasons

def prepared_identity(directory, execution_binary=None):
    path = directory / "build-metadata.json"
    metadata = json.loads(path.read_text())
    if metadata["protocol_sha256"] != sha(PROTOCOL):
        raise RuntimeError("prepared protocol changed")
    if metadata["binary_sha256"] != sha(ROOT / metadata["binary"]):
        raise RuntimeError("prepared benchmark executable changed")
    if execution_binary is not None and sha(execution_binary) != metadata["binary_sha256"]:
        raise RuntimeError("requested executable differs from prepared binary")
    for field in ("input_hashes", "source_hashes"):
        for name, expected in metadata[field].items():
            if sha(ROOT / name) != expected:
                raise RuntimeError("prepared source/input changed: " + name)
    if metadata["retained_corpus_hashes"] != PINNED:
        raise RuntimeError("prepared corpus identity mismatch")
    return {"metadata_sha256": sha(path), "protocol_sha256": sha(PROTOCOL),
            "binary_sha256": metadata["binary_sha256"]}

def summarize(directory):
    policy = json.loads(PROTOCOL.read_text())
    identity = prepared_identity(directory)
    expected_counts = {f"mixed-{n}": n for n in (0, 1, 31, 33)}
    expected_counts.update({f"synthetic-psm{p}-{n}": n for n in (4096, 16384, 143360, 1048576) for p in (0, 19)})
    expected_counts["retained-corpus-356160"] = 356160
    grouped = {}
    for trial in range(policy["trials"]):
        eligibility = json.loads((directory / f"trial-{trial}-eligibility.json").read_text())
        if eligibility["identity"] != identity:
            raise RuntimeError("mixed trial source/build/protocol identities")
        if not eligibility["eligible"] or eligibility["reasons"]:
            raise RuntimeError("contaminated trial cannot support verdict; retain and rerun same policy")
        path = directory / f"trial-{trial}.jsonl"
        if eligibility["raw_sha256"] != sha(path):
            raise RuntimeError("raw trial changed after eligibility review")
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        correctness = [r for r in rows if r["type"] == "correctness"]
        if correctness != [{"type": "correctness", "rows": 356160, "different_results": 0}]:
            raise RuntimeError("missing trial corpus correctness")
        samples = [r for r in rows if r["type"] == "sample"]
        if len(samples) != 13 * policy["pairs"]:
            raise RuntimeError("missing workload samples")
        names = {r["workload"] for r in samples}
        if names != set(expected_counts):
            raise RuntimeError("preselected workload coverage mismatch")
        for name in names:
            values = [r for r in samples if r["workload"] == name]
            if len(values) != 31 or {r["pair"] for r in values} != set(range(31)):
                raise RuntimeError("pair mismatch")
            for r in values:
                if r["count"] != expected_counts[name]:
                    raise RuntimeError("workload count mismatch")
                if r["trial"] != trial or r["cpu_first"] != ((r["pair"] + trial) % 2 == 0):
                    raise RuntimeError("trial/order mismatch")
                if r["input_bytes"] != r["count"] * 20 or r["output_bytes"] != r["count"] * 8:
                    raise RuntimeError("byte extent mismatch")
                if not math.isfinite(r["cpu_s"]) or not math.isfinite(r["gpu_e2e_s"]) or r["cpu_s"] < 0 or r["gpu_e2e_s"] <= 0:
                    raise RuntimeError("invalid wall timing")
                if r["gpu_command_s"] is not None and (not math.isfinite(r["gpu_command_s"]) or r["gpu_command_s"] <= 0):
                    raise RuntimeError("invalid GPU command timestamp duration")
            grouped.setdefault(name, []).append(values)
    summary = []
    for name, trials in grouped.items():
        if len(trials) != 5:
            raise RuntimeError("workload missing from trial")
        ratios = [statistics.median([r["cpu_s"] for r in t]) /
                  statistics.median([r["gpu_e2e_s"] for r in t]) for t in trials]
        all_rows = [r for t in trials for r in t]
        cpu = [r["cpu_s"] for r in all_rows]
        gpu = [r["gpu_e2e_s"] for r in all_rows]
        command = [r["gpu_command_s"] for r in all_rows if r["gpu_command_s"] is not None]
        passes = sum(r >= 1.2 for r in ratios)
        eligible = all_rows[0]["count"] in policy["success"]["eligible_counts"]
        summary.append({"workload": name, "count": all_rows[0]["count"],
                        "cpu_median_s": statistics.median(cpu), "cpu_p95_s": percentile95(cpu),
                        "gpu_e2e_median_s": statistics.median(gpu), "gpu_e2e_p95_s": percentile95(gpu),
                        "gpu_command_median_s": statistics.median(command) if command else None,
                        "gpu_command_p95_s": percentile95(command) if command else None,
                        "trial_ratios": ratios, "aggregate_ratio": statistics.median(ratios),
                        "passing_trials": passes, "criterion_pass": eligible and passes >= 4 and statistics.median(ratios) >= 1.2})
    summary.sort(key=lambda r: (r["count"], r["workload"]))
    return {"workloads": summary, "speedup_demonstrated": any(r["criterion_pass"] for r in summary),
            "statistics": "pooled wall median/p95; verdict uses median of five per-trial ratios; p95 nearest rank"}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["prepare", "measure", "summarize"])
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/gpu-address-release/okage-gpu-address-benchmark")
    args = parser.parse_args()
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    for name, expected in PINNED.items():
        if sha(CORPUS / name) != expected:
            raise RuntimeError("retained corpus provenance mismatch")
    policy = json.loads(PROTOCOL.read_text())
    if (policy["warmups"], policy["pairs"], policy["trials"], policy["calls_per_sample"]) != (10, 31, 5, 1):
        raise RuntimeError("protocol and executable disagree")
    binary = args.binary.resolve()
    files = [str(CORPUS / "addresses.bin"), str(CORPUS / "play-addresses-v1.bin")]
    if args.action == "prepare":
        run([str(binary), "--dump-inputs", str(directory / "inputs"), *files], directory / "dump-inputs.jsonl")
        metadata = {"protocol_sha256": sha(PROTOCOL), "binary_sha256": sha(binary), "binary": str(binary),
                    "source_revision": capture(["git", "rev-parse", "HEAD"]),
                    "source_status": capture(["git", "status", "--short"]),
                    "os": capture(["sw_vers"]), "cpu": capture(["sysctl", "-n", "machdep.cpu.brand_string"]),
                    "cpu_cores": capture(["sysctl", "-n", "hw.ncpu"]),
                    "memory_bytes": capture(["sysctl", "-n", "hw.memsize"]),
                    "sdk": capture(["xcrun", "--show-sdk-version"]),
                    "compiler": capture(["clang++", "--version"]), "cpu_threads": 1,
                    "power": capture(["pmset", "-g", "custom"]),
                    "input_hashes": {str(p.relative_to(ROOT)): sha(p) for p in sorted((directory / "inputs").glob("*"))},
                    "retained_corpus_hashes": PINNED}
        source = (ROOT / "src/texture_address_batch_metal.mm").read_text()
        shader = source.split('R"OKAGE_METAL(', 1)[1].split(')OKAGE_METAL"', 1)[0].encode()
        (directory / "shader.metal").write_bytes(shader)
        metadata["shader_sha256"] = hashlib.sha256(shader).hexdigest()
        source_paths = ["CMakeLists.txt", "include/okage/texture_address_batch.hpp", "src/gs_texture_address.cpp",
                        "include/okage/gs_texture_address.hpp", "src/texture_address_batch.cpp", "src/texture_address_batch_metal.mm",
                        "tools/gpu_address_benchmark.cpp", "tools/run_gpu_address_poc.py", "tests/texture_address_batch_tests.cpp",
                        "tests/texture_address_batch_cases.hpp", "tests/test_gpu_address_poc.py"]
        metadata["source_hashes"] = {p: sha(ROOT / p) for p in source_paths}
        compile_commands = json.loads((binary.parent / "compile_commands.json").read_text())
        metadata["compile_commands"] = [r for r in compile_commands if Path(r["file"]).name in
                                        ("gs_texture_address.cpp", "texture_address_batch.cpp", "texture_address_batch_metal.mm", "gpu_address_benchmark.cpp")]
        (directory / "build-metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    elif args.action == "measure":
        if list(directory.glob("trial-*")):
            raise RuntimeError("measurement observations already exist; use a fresh prepared attempt directory")
        identity = prepared_identity(directory, binary)
        for trial in range(5):
            if prepared_identity(directory) != identity:
                raise RuntimeError("prepared identity changed between trials")
            before = observe()
            (directory / f"trial-{trial}-before.json").write_text(json.dumps(before, indent=2) + "\n")
            reasons = load_reasons(before)
            if before["hid_idle_seconds"] is None or before["hid_idle_seconds"] < 10:
                reasons.append("no confirmed ten-second input-idle interval before trial")
            if reasons:
                (directory / f"trial-{trial}-preflight-rejection.json").write_text(json.dumps(reasons, indent=2) + "\n")
                raise RuntimeError("measurement deferred by observed load/interaction; see retained preflight")
            run([str(binary), "--trial", str(trial), *files], directory / f"trial-{trial}.jsonl")
            after = observe()
            (directory / f"trial-{trial}-after.json").write_text(json.dumps(after, indent=2) + "\n")
            reasons = load_reasons(after)
            elapsed = after["monotonic"] - before["monotonic"]
            if after["hid_idle_seconds"] is None or after["hid_idle_seconds"] + .5 < before["hid_idle_seconds"] + elapsed:
                reasons.append("input-idle clock reset during trial")
            eligibility = {"eligible": not reasons, "reasons": reasons, "identity": identity,
                           "raw_sha256": sha(directory / f"trial-{trial}.jsonl"),
                           "limits": "before/after observations; not a profiler or proof against every short transient"}
            (directory / f"trial-{trial}-eligibility.json").write_text(json.dumps(eligibility, indent=2) + "\n")
            if reasons:
                raise RuntimeError("trial contaminated; retained raw output; rerun in fresh attempt directory")
    else:
        result = summarize(directory)
        (directory / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
        for r in result["workloads"]:
            print(f'{r["workload"]}: CPU {r["cpu_median_s"]*1e6:.3f} us, GPU {r["gpu_e2e_median_s"]*1e6:.3f} us, ratio {r["aggregate_ratio"]:.3f}, pass={r["criterion_pass"]}')
        print("speedup_demonstrated:", result["speedup_demonstrated"])

if __name__ == "__main__":
    main()
