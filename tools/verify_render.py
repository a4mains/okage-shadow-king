#!/usr/bin/env python3
"""Bounded rendering contracts: original VU1 emission and EE UV conversion.

Synthetic cached vertices isolate primitive order/culling/STQ output. The UV
oracle executes original 0x17e8e0 with a prepared packet context, not a formula
stub. This does not compare rendered images, clipping, lighting, or full scenes.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import random
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def write_once(path, data):
    if path.is_symlink() or (path.exists() and path.read_bytes() != data):
        raise ValueError(f"Refusing different existing fixture: {path}")
    path.write_bytes(data)

def check_primitive(data, count, textured, reverse, clip_checks):
    qwords = 1 + count * 3
    if len(data) != 4 + qwords * 16:
        raise ValueError("Wrong primitive dump size")
    emitted = struct.unpack_from("<I", data)[0]
    if emitted != (0 if reverse else qwords):
        raise ValueError("Wrong primitive count/culling")
    if reverse:
        expected = bytearray(b"\xcd" * (qwords * 16))
        if count == 3 and not textured and not clip_checks:
            # SQ at 0x2668 is the culling branch delay slot. It writes the first
            # position speculatively; no header or output cursor is committed.
            expected[3*16:4*16] = struct.pack("<4i", 256,512,4,16)
        if data[4:] != expected:
            raise ValueError("Unexpected speculative writes for culled primitive")
        return
    prim = 0x5d if textured else 0x4d
    expected = struct.pack("<4I", count, 0x30004000 | prim << 15 | prim,
                           0x412 if textured else 0x41f, 0)
    positions = [(16, 32, .25, 1), (80, 32, .5, 1), (80, 96, 1, 1), (16, 96, 2, 1)]
    for i in range(count):
        q = positions[i][2]
        expected += (struct.pack("<4f", (i+1)*.125*q, (i+2)*.125*q, q, 0)
                     if textured else b"\xcd" * 16)
        expected += struct.pack("<4f", (i+1)*2+.5, (i+2)*3+.5, (i+3)*4+.5, (i+4)*5+.5)
        expected += struct.pack("<4i", *(int(v*16) for v in positions[i]))
    if data[4:] != expected:
        raise ValueError("VU1 output differs from recovered order/STQ/color/position contract")

def compare_uv(reference, native, count):
    if len(reference) != count * 8 or len(native) != len(reference):
        raise ValueError("Wrong UV output size")
    if reference != native:
        for at in range(0, len(reference), 4):
            if reference[at:at+4] != native[at:at+4]:
                a, b = (struct.unpack_from("<f", x, at)[0] for x in (reference, native))
                raise ValueError(f"UV mismatch record {at//8}, axis {at//4%2}: {a} != {b}")

def synthetic_uv():
    result = bytearray()
    for dims, modes, region, coord in itertools.product(
            [(64,32), (1,1), (320,240), (4095,4095)],
            [(1,1), (0,0), (-1,-7), (2,3), (1,4), (4,1)],
            [(0,0,1,1), (.125,.25,.75,1), (8,4,24,12), (0,0,1.00001,1), (-.5,-.5,1,1)],
            [(0,0), (1,1), (-1,2), (.1,.3), (.999999,1.000001)]):
        result += struct.pack("<IIii6f", *dims, *modes, *region, *coord)
    rng = random.Random(0x97129)
    for _ in range(2048):
        result += struct.pack("<IIii6f", rng.randint(1,4095), rng.randint(1,4095),
                              rng.choice([-32768,-1,0,1,2,3,16,32767]),
                              rng.choice([-32768,-1,0,1,2,3,16,32767]),
                              *(rng.uniform(-2,3) for _ in range(4)),
                              rng.uniform(-10,10), rng.uniform(-10,10))
    return bytes(result)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "local/render-validation")
    parser.add_argument("--native", type=Path, default=ROOT / "build/native/okage-assets")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    elf = ROOT / "local/extracted/SCUS_971.29"
    oracle = ROOT / "build/play-research/okage-oracle/okage-pose-oracle"
    report = dict(elf_sha256=sha(elf), oracle_sha256=sha(oracle), native_sha256=sha(args.native),
                  play_revision=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT / "references/Play", text=True).strip(),
                  primitive_cases=[], uv_cases=[], clamp_cases=[],
                  scope="Unclipped synthetic VU1 packets and singleton nonanimated EE UV conversion; no rendered-frame comparison")
    for count, tex, reverse, clip in itertools.product((3,4), (0,1), (0,1), (0,1)):
        out = args.output / f"primitive-{count}-{tex}-{reverse}-{clip}.bin"
        subprocess.run([str(oracle), "primitive", str(elf), str(count), str(tex), str(reverse), str(clip), str(out)], cwd=ROOT, check=True)
        check_primitive(out.read_bytes(), count, tex, reverse, clip)
        report["primitive_cases"].append(dict(count=count, textured=bool(tex), reverse=bool(reverse),
                                               clip_checks=bool(clip), sha256=sha(out)))
    synth = args.output / "synthetic.cases"
    write_once(synth, synthetic_uv())
    inputs = [("synthetic", synth, None)]
    for name, relative in [("hero", "FLDDATA/OBJ/HERO.XPF/a.xmd"), ("bag", "BTLDATA/ALPHA/EFFECT/BAG.XPF/a.xmd")]:
        model = ROOT / "local/xpf-decoded" / relative
        out = args.output / f"{name}.cases"
        subprocess.run([str(args.native), "uv-model-cases", str(model), str(out)], cwd=ROOT, check=True)
        inputs.append((name, out, sha(model)))
    for name, cases, model_hash in inputs:
        ref, native = (args.output / f"{name}.{suffix}" for suffix in ("retail.uv", "native.uv"))
        subprocess.run([str(oracle), "uv", str(elf), str(cases), str(ref)], cwd=ROOT, check=True)
        subprocess.run([str(args.native), "uv-cases", str(cases), str(native)], cwd=ROOT, check=True)
        count = cases.stat().st_size // 40
        compare_uv(ref.read_bytes(), native.read_bytes(), count)
        report["uv_cases"].append(dict(name=name, count=count, input_sha256=sha(cases),
                                       model_sha256=model_hash, reference_sha256=sha(ref), native_sha256=sha(native)))
        print(f"{name}: {count} UV pairs match exactly", flush=True)
        ref, native = (args.output / f"{name}.{suffix}" for suffix in ("retail.clamp", "native.clamp"))
        subprocess.run([str(oracle), "texture-clamp", str(elf), str(cases), str(ref)], cwd=ROOT, check=True)
        subprocess.run([str(args.native), "texture-clamp-cases", str(cases), str(native)], cwd=ROOT, check=True)
        if len(ref.read_bytes()) != count*8 or ref.read_bytes() != native.read_bytes():
            raise ValueError(f"Texture clamp register mismatch: {name}")
        report["clamp_cases"].append(dict(name=name, count=count, input_sha256=sha(cases),
                                          reference_sha256=sha(ref), native_sha256=sha(native)))
        print(f"{name}: {count} CLAMP register values match exactly", flush=True)
    report["passed"] = True
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")

if __name__ == "__main__":
    main()
