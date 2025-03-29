#!/usr/bin/env python3
"""Generate the TXTT v2 input read by `okage-pose-oracle texture-transfer`
(Startup PSM20/36/44 format transfers) plus a JSON case catalogue.

Usage: texture_transfer_cases_v2.py input.bin catalogue.json

Row layout (12 little-endian u32 words, see tools/play-oracle/main.cpp
textureTransfer2): kind (0 allocate+upload, 2 synthetic descriptor), psm, w, h,
x, y, tbp, tbw, pattern, seed, pre_lo, pre_len.  The case list is code, not
data, so the coverage claims in the startup texture-transfer coverage contract are checkable
against it.  Refuses to overwrite its outputs."""
import json
import os
import struct
import sys

PSM = {"t4": 20, "t4hl": 36, "t4hh": 44, "ct32": 0, "t8": 19}
FOUR_BIT = ("t4", "t4hl", "t4hh")
ALL = tuple(PSM)
PATTERN_NAMES = {0: "hash", 1: "all-00", 2: "all-ff", 3: "f0/0f"}

def cases():
    out = []

    def add(name, fmt, kind, w, h, x=0, y=0, tbp=0, tbw=0, pattern=0, pre_lo=0, pre_len=0, tags=()):
        seed = len(out) + 1
        out.append(dict(name=f"{fmt}:{name}", fmt=fmt, row=(kind, PSM[fmt], w, h, x, y, tbp, tbw, pattern, seed,
                                                           pre_lo, pre_len), tags=list(tags)))

    # Allocated images (descriptor TBP/TBW/id from unchanged 1cf008).
    for fmt in FOUR_BIT:
        for w, h, tag in ((64, 64, "aligned"), (128, 128, "one-page"), (100, 30, "odd-tail"), (33, 17, "odd-tail"),
                          (5, 7, "one-qword"), (200, 140, "multi-page"), (256, 64, "wide")):
            add(f"alloc-{w}x{h}", fmt, 0, w, h, tags=("allocated", tag))
    for w, h, tag in ((64, 64, "aligned"), (3, 3, "floor-tail"), (17, 17, "odd-tail")):
        add(f"alloc-{w}x{h}", "ct32", 0, w, h, tags=("allocated", "regression", tag))
    for w, h, tag in ((64, 64, "aligned"), (5, 5, "floor-tail"), (100, 3, "odd-tail")):
        add(f"alloc-{w}x{h}", "t8", 0, w, h, tags=("allocated", "regression", tag))

    # Synthetic descriptors: offsets, TBW stride, odd TBW page rows, wrap.
    dims = {"t4": (300, 300), "t4hl": (300, 300), "t4hh": (300, 300), "ct32": (96, 100), "t8": (200, 150)}
    for fmt in ALL:
        reg = ("regression",) if fmt in ("ct32", "t8") else ()
        big_w, big_h = dims[fmt]
        add("offset-rect", fmt, 2, 40, 20, x=5, y=3, tbp=0x100, tbw=3, tags=("synthetic", "dsax-dsay") + reg)
        add("page-cross", fmt, 2, 40, 20, x=120, y=125, tbp=0x200, tbw=2, tags=("synthetic", "page-cross") + reg)
        add("odd-tbw-pagerows", fmt, 2, big_w, big_h, tbp=0x40, tbw=3, tags=("synthetic", "odd-tbw", "pages") + reg)
        add("even-tbw-pagerows", fmt, 2, big_w, big_h, tbp=0x40, tbw=4, tags=("synthetic", "even-tbw", "pages") + reg)
        add("tbw1", fmt, 2, 64, 70, tbp=0x80, tbw=1, tags=("synthetic", "tbw", "pages") + reg)
        add("tbw0-alias", fmt, 2, 64, 140, tbp=0x80, tbw=0, tags=("synthetic", "tbw0") + reg)
        add("odd-tail-offset", fmt, 2, 35, 5, x=7, y=9, tbp=0x300, tbw=1, tags=("synthetic", "odd-tail", "dsax-dsay") + reg)
        add("wrap-4mib", fmt, 2, big_w, 140 if fmt not in ("ct32",) else 60, tbp=0x3fe0, tbw=5,
            tags=("synthetic", "wrap", "pages") + reg)
        add("wrap-unaligned-tbp", fmt, 2, 64, 40, tbp=0x3fff, tbw=2, tags=("synthetic", "wrap") + reg)
        add("high-origin", fmt, 2, 48, 40, x=2000, y=2000, tbp=0x500, tbw=32, tags=("synthetic", "dsax-dsay") + reg)
        add("tbp-top", fmt, 2, 64, 64, tbp=0x3f00, tbw=1, tags=("synthetic", "wrap") + reg)
        # Pre-existing neighbouring data: window covers and exceeds the written area.
        for pattern in (0, 1, 2, 3):
            add(f"preload-{PATTERN_NAMES[pattern]}", fmt, 2, 70, 30, x=3, y=2, tbp=0x60, tbw=2, pattern=pattern,
                pre_lo=0x6000 - 0x400, pre_len=0x5000, tags=("synthetic", "preload", "neighbours") + reg)
        add("payload-all-00", fmt, 2, 64, 64, tbp=0x90, tbw=1, pattern=1, tags=("synthetic", "fill-equal-00") + reg)
        add("payload-all-ff", fmt, 2, 64, 64, tbp=0x90, tbw=1, pattern=2, tags=("synthetic", "fill-equal-ff") + reg)
        add("payload-f0-0f", fmt, 2, 64, 64, tbp=0x90, tbw=1, pattern=3, tags=("synthetic", "both-nibbles") + reg)
        add("preload-wrap", fmt, 2, 128, 40, tbp=0x3ff0, tbw=2, pre_lo=0x3ff000, pre_len=0x1000,
            tags=("synthetic", "preload", "wrap") + reg)
    return out

def main(argv):
    if len(argv) != 3:
        print(__doc__.splitlines()[2], file=sys.stderr)
        return 2
    for p in argv[1:]:
        if os.path.lexists(p):
            print(f"refusing to overwrite {p}", file=sys.stderr)
            return 1
    cs = cases()
    data = struct.pack("<4I", 0x54545854, 2, len(cs), 12)
    for c in cs:
        data += struct.pack("<12I", *c["row"])
    with open(argv[1], "xb") as f:
        f.write(data)
    cat = [dict(case=i, name=c["name"], fmt=c["fmt"], row=c["row"], tags=c["tags"]) for i, c in enumerate(cs)]
    with open(argv[2], "x") as f:
        json.dump({"row_fields": ["kind", "psm", "w", "h", "x", "y", "tbp", "tbw", "pattern", "seed", "pre_lo", "pre_len"],
                   "cases": cat}, f, indent=1)
    print(f"{len(cs)} cases")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
