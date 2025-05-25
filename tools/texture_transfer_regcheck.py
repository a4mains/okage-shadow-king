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
Title: PSM1 uses 24 bits/pixel. Staged cases (kinds "staged"/"allocated-pair") issue two
uploads: the first register set / IMAGE packet is the pre-upload (psm pre_lo, pre_len =
w | h << 16, pattern 0, seed + 1000, origin 0,0, tbp/tbw from pre_descriptor), the second
the main transfer.
Exit 0 when every case agrees, 1 otherwise, 2 on usage/malformed input."""
import hashlib
import json
import sys

BITS = {0: 32, 1: 24, 19: 8, 20: 4, 36: 4, 44: 4}

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

def check_upload(regs, image_total, image_hashes, psm, tbp, tbw, x, y, w, h, pattern, seed, idx, tag, problems):
    bits = BITS[psm]
    nbytes = (w * h * bits + 7) // 8
    qwc = (w * h * bits) // 128

    def one(reg):
        v = regs.get(reg, [])
        if len(v) <= idx:
            problems.append(f"{tag}register {reg:#x} written {len(v)} times")
            return 0, 0
        return v[idx]
    lo, hi = one(0x50)
    if lo != 0:
        problems.append(f"{tag}BITBLTBUF source fields {lo:#x} not zero")
    if (hi & 0x3FFF, (hi >> 16) & 0x3F, (hi >> 24) & 0x3F, hi >> 30) != (tbp, tbw, psm, 0):
        problems.append(f"{tag}BITBLTBUF dst {hi:#x} != tbp {tbp} tbw {tbw} psm {psm}")
    lo, hi = one(0x51)
    if lo != 0 or (hi & 0x7FF, (hi >> 16) & 0x7FF, hi >> 27) != (x, y, 0):
        problems.append(f"{tag}TRXPOS {lo:#x}/{hi:#x} != x {x} y {y}")
    lo, hi = one(0x52)
    if (lo, hi) != (w, h):
        problems.append(f"{tag}TRXREG {lo}/{hi} != {w}x{h}")
    lo, hi = one(0x53)
    if (lo, hi) != (0, 0):
        problems.append(f"{tag}TRXDIR {lo}/{hi} != 0")
    if image_total[idx] != qwc * 16:
        problems.append(f"{tag}IMAGE bytes {image_total[idx]} != floor qwc {qwc} * 16")
    expect = hashlib.sha256(payload(pattern, seed, nbytes)[: qwc * 16]).hexdigest()
    if image_hashes[idx] != expect:
        problems.append(f"{tag}IMAGE data is not the first qwc*16 payload bytes")

def check(c):
    problems = []
    staged = c.get("kind") in ("staged", "allocated-pair")
    regs = {}
    image_total = []
    image_hashes = []
    for p in c["packets"]:
        for r, lo, hi in p["register_writes"]:
            regs.setdefault(r, []).append((lo, hi))
        if p["image_bytes"]:
            image_total.append(p["image_bytes"])
            image_hashes.append(p["image_sha256"])
    expected_uploads = 2 if staged else 1
    if len(image_total) != expected_uploads:
        problems.append(f"{len(image_total)} IMAGE packets, expected {expected_uploads}")
        return problems
    idx = 0
    if staged:
        pd = c["pre_descriptor"]
        ptbp, ptbw, ppsm = pd[0] & 0x3FFF, (pd[0] >> 14) & 0x3F, pd[1] & 0x3F
        if ppsm != c["pre_lo"]:
            problems.append(f"pre descriptor psm {ppsm} != pre_lo {c['pre_lo']}")
        check_upload(regs, image_total, image_hashes, ppsm, ptbp, ptbw, 0, 0, c["pre_len"] & 0xFFFF,
                     c["pre_len"] >> 16, 0, c["seed"] + 1000, 0, "pre: ", problems)
        idx = 1
    check_upload(regs, image_total, image_hashes, c["psm"], c["tbp"], c["tbw"], c["x"], c["y"], c["w"], c["h"],
                 c["pattern"], c["seed"], idx, "", problems)
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
