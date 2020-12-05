#!/usr/bin/env python3
"""Compare native XMT channels and poses with original EE/VU execution.

This is a bounded HERO motion experiment, not whole-corpus/game validation.
Both executables refuse replacing different existing dump files.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def compare(reference, native, channels, nodes, frames):
    a, b = reference.read_bytes(), native.read_bytes()
    stride = 8 + channels * 16 + nodes * 64
    if len(a) != stride * (frames + 1) or len(b) != len(a):
        raise ValueError("Unexpected animation dump size")
    result = dict(state_mismatches=0, channel_bit_mismatches=0,
                  pose_components=(frames + 1) * nodes * 16,
                  pose_numerically_equal=0, pose_outside_tolerance=0,
                  pose_max_absolute_error=0.0)
    for frame in range(frames + 1):
        at = frame * stride
        result["state_mismatches"] += a[at:at+8] != b[at:at+8]
        at += 8
        for offset in range(at, at + channels * 16, 4):
            result["channel_bit_mismatches"] += a[offset:offset+4] != b[offset:offset+4]
        at += channels * 16
        for component in range(nodes * 16):
            x = struct.unpack_from("<f", a, at + component * 4)[0]
            y = struct.unpack_from("<f", b, at + component * 4)[0]
            error = abs(x - y)
            result["pose_numerically_equal"] += x == y
            if not math.isfinite(error) or error > 2e-5 + 2e-6 * abs(x):
                result["pose_outside_tolerance"] += 1
            if error > result["pose_max_absolute_error"]:
                result["pose_max_absolute_error"] = error
                result["worst_pose"] = dict(frame=frame, component=component, reference=x, native=y)
    result["passed"] = not any(result[k] for k in
                               ("state_mismatches", "channel_bit_mismatches", "pose_outside_tolerance"))
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--entries", nargs="+", type=int, default=[3, 20, 21])
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--native", type=Path, default=ROOT / "build/native/okage-assets")
    parser.add_argument("--output", type=Path, default=ROOT / "local/animation-validation-v2")
    args = parser.parse_args()
    if args.frames < 0 or args.frames > 10000 or any(i < 0 for i in args.entries):
        parser.error("Invalid frame count or entry index")
    elf = ROOT / "local/extracted/SCUS_971.29"
    base = ROOT / "local/xpf-decoded/FLDDATA/OBJ/HERO.XPF"
    xmt, xmd = base / "a.xmt", base / "a.xmd"
    oracle = ROOT / "build/play-research/okage-oracle/okage-pose-oracle"
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(scope="HERO XMT channel and animated-pose reference experiment",
                  elf_sha256=sha(elf), xmt_sha256=sha(xmt), xmd_sha256=sha(xmd),
                  native_sha256=sha(args.native), oracle_sha256=sha(oracle),
                  channels=228, hierarchy_nodes=112, samples_per_entry=args.frames+1,
                  channel_comparison="bit exact", script_state_comparison="exact",
                  pose_tolerance="abs(error) <= 2e-5 + 2e-6 * abs(reference)", entries=[])
    for entry in args.entries:
        ref = args.output / f"hero-{entry}-retail.anim"
        native = args.output / f"hero-{entry}-native.anim"
        subprocess.run([str(oracle), "animation", str(elf), str(xmt), "228", str(entry),
                        str(args.frames), str(ref), str(xmd)], cwd=ROOT, check=True)
        subprocess.run([str(args.native), "animation", str(xmt), "228", str(entry),
                        str(args.frames), str(native), str(xmd)], cwd=ROOT, check=True)
        result = compare(ref, native, 228, 112, args.frames)
        result.update(entry=entry, reference_sha256=sha(ref), native_sha256=sha(native))
        report["entries"].append(result)
        print(json.dumps(result), flush=True)
    report["passed"] = all(entry["passed"] for entry in report["entries"])
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if not report["passed"]:
        raise SystemExit("Animation comparison failed")

if __name__ == "__main__":
    main()
