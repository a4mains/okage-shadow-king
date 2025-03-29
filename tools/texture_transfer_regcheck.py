#!/usr/bin/env python3
"""Cross-check the GIF register writes and IMAGE data retained from unchanged
retail 0x1cba48 -> 0x1c9bb8 against the documented register encoding and the
floor-QWC rule (observer version 2, tools/play-oracle textureTransfer2).

Usage: texture_transfer_regcheck.py transfer.json

For every case it requires, independently of the native transfer code:
  BITBLTBUF (0x50): DBP=tbp, DBW=tbw, DPSM=psm, source fields zero
  TRXPOS    (0x51): DSAX=x, DSAY=y, direction bits zero
  TRXREG    (0x52): RRW=w, RRH=h
  TRXDIR    (0x53): 0 (host to local)
  IMAGE data      : exactly floor(w*h*bits/128) quadwords, equal (SHA-256) to the first
                    qwc*16 bytes of the regenerated payload
Exit 0 when every case agrees, 1 otherwise, 2 on usage/malformed input."""
import hashlib
import json
import sys

BITS = {0: 32, 19: 8, 20: 4, 36: 4, 44: 4}

def payload(pattern, seed, n):
    out = bytearray(n)
    for k in range(n):
        if pattern == 0:
            out[k] = (k * 167 + seed * 29 + 5) & 255
        elif pattern == 1:
            out[k] = 0
        elif pattern == 2:
            out[k] = 255
        else:
            out[k] = 0x0F if k & 1 else 0xF0
    return bytes(out)

def check(c):
    problems = []
    bits = BITS[c["psm"]]
    nbytes = (c["w"] * c["h"] * bits + 7) // 8
    qwc = (c["w"] * c["h"] * bits) // 128
    packets = c["packets"]
    regs = {}
    image_total = 0
    image_hashes = []
    for p in packets:
        for r, lo, hi in p["register_writes"]:
            regs.setdefault(r, []).append((lo, hi))
        if p["image_bytes"]:
            image_total += p["image_bytes"]
            image_hashes.append(p["image_sha256"])
    def one(reg):
        v = regs.get(reg, [])
        if len(v) != 1:
            problems.append(f"register {reg:#x} written {len(v)} times")
            return 0, 0
        return v[0]
    lo, hi = one(0x50)
    if lo != 0:
        problems.append(f"BITBLTBUF source fields {lo:#x} not zero")
    if (hi & 0x3FFF, (hi >> 16) & 0x3F, (hi >> 24) & 0x3F, hi >> 30) != (c["tbp"], c["tbw"], c["psm"], 0):
        problems.append(f"BITBLTBUF dst {hi:#x} != tbp {c['tbp']} tbw {c['tbw']} psm {c['psm']}")
    lo, hi = one(0x51)
    if lo != 0 or (hi & 0x7FF, (hi >> 16) & 0x7FF, hi >> 27) != (c["x"], c["y"], 0):
        problems.append(f"TRXPOS {lo:#x}/{hi:#x} != x {c['x']} y {c['y']}")
    lo, hi = one(0x52)
    if (lo, hi) != (c["w"], c["h"]):
        problems.append(f"TRXREG {lo}/{hi} != {c['w']}x{c['h']}")
    lo, hi = one(0x53)
    if (lo, hi) != (0, 0):
        problems.append(f"TRXDIR {lo}/{hi} != 0")
    if image_total != qwc * 16:
        problems.append(f"IMAGE bytes {image_total} != floor qwc {qwc} * 16")
    expect = hashlib.sha256(payload(c["pattern"], c["seed"], nbytes)[: qwc * 16]).hexdigest()
    if image_hashes != [expect]:
        problems.append("IMAGE data is not the first qwc*16 payload bytes")
    return problems

def main(argv):
    if len(argv) != 2:
        print(__doc__.splitlines()[0], file=sys.stderr)
        return 2
    try:
        with open(argv[1], "rb") as f:
            doc = json.load(f)
        if doc.get("schema") != "okage-texture-transfer-v2":
            raise ValueError("not an okage-texture-transfer-v2 report")
        cases = doc["cases"]
        bad = 0
        for c in cases:
            probs = check(c)
            if probs:
                bad += 1
                print(f"case {c['case']} psm{c['psm']} {c['w']}x{c['h']}: DIFF - {'; '.join(probs)}")
    except (OSError, ValueError, KeyError, TypeError) as e:
        print(f"input error: {e}", file=sys.stderr)
        return 2
    print(f"{len(cases)} cases, {bad} register/QWC differences")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
