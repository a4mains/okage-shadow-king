#!/usr/bin/env python3
"""Generate the TXTT v2 input read by `okage-pose-oracle texture-transfer`
(Startup PSM20/36/44 format transfers) plus a JSON case catalogue.

Usage: texture_transfer_cases_v2.py input.bin catalogue.json

Row layout (12 little-endian u32 words, see tools/play-oracle/main.cpp
textureTransfer2): kind (0 allocate+upload, 2 synthetic descriptor, 3 synthetic
descriptor after a staged pre-upload, 4 two allocations then pre-upload and main
upload), psm, w, h, x, y, tbp, tbw, pattern, seed, pre_lo, pre_len.  For kinds 3/4
pre_lo is the pre-upload PSM and pre_len = pre_w | pre_h << 16.  The case list
is code, not data, so the coverage claims in the startup texture-transfer coverage contract are checkable against it.  The first 122 cases are the
Startup corpus unchanged (same order and seeds); the PSM1 cases (Title) follow.  Refuses to overwrite its outputs."""
import json
import os
import struct
import sys

PSM = {"t4": 20, "t4hl": 36, "t4hh": 44, "ct32": 0, "t8": 19, "ct24": 1}
FOUR_BIT = ("t4", "t4hl", "t4hh")
ALL = ("t4", "t4hl", "t4hh", "ct32", "t8")
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
    psm1_cases(add)
    return out

def psm1_cases(add):
    """Title PSM1 (PSMCT24) cases, appended after the Startup corpus."""
    F = "ct24"
    for w, h, tag in ((64, 64, "aligned"), (128, 128, "one-page"), (100, 30, "odd-tail"), (33, 17, "dropped-pixel"),
                      (5, 7, "dropped-pixels"), (200, 140, "multi-page"), (256, 64, "wide"), (16, 16, "one-block"),
                      (3, 3, "partial-pixel-tail"), (6, 1, "partial-pixel-tail"), (11, 1, "partial-pixel-tail"),
                      (7, 3, "partial-pixel-tail"), (17, 17, "odd-tail"), (32, 32, "xim-size"), (128, 256, "xim-size"),
                      (64, 128, "xim-size"),
                      (32, 64, "xim-size"), (64, 32, "xim-size"), (32, 16, "xim-size"), (64, 16, "xim-size"),
                      (128, 16, "xim-size")):
        add(f"alloc-{w}x{h}", F, 0, w, h, tags=("allocated", tag))
    # Synthetic descriptors: offsets, TBW stride, odd TBW page rows, wrap, addresses past 2047.
    add("offset-rect", F, 2, 40, 20, x=5, y=3, tbp=0x100, tbw=3, tags=("synthetic", "dsax-dsay"))
    add("page-cross", F, 2, 40, 20, x=120, y=125, tbp=0x200, tbw=2, tags=("synthetic", "page-cross"))
    add("odd-tbw-pagerows", F, 2, 200, 150, tbp=0x40, tbw=3, tags=("synthetic", "odd-tbw", "pages"))
    add("even-tbw-pagerows", F, 2, 200, 150, tbp=0x40, tbw=4, tags=("synthetic", "even-tbw", "pages"))
    add("tbw1", F, 2, 64, 70, tbp=0x80, tbw=1, tags=("synthetic", "tbw", "pages"))
    add("tbw0-alias", F, 2, 64, 140, tbp=0x80, tbw=0, tags=("synthetic", "tbw0"))
    add("stride-wider-than-rect", F, 2, 33, 9, tbp=0x80, tbw=5, tags=("synthetic", "tbw"))
    add("odd-tail-offset", F, 2, 35, 5, x=7, y=9, tbp=0x300, tbw=1, tags=("synthetic", "odd-tail", "dsax-dsay"))
    add("dropped-tail-offset", F, 2, 21, 5, x=9, y=125, tbp=0x300, tbw=1, tags=("synthetic", "dropped-pixel", "page-cross"))
    add("partial-pixel-tail", F, 2, 6, 1, x=63, y=63, tbp=0x340, tbw=1, tags=("synthetic", "partial-pixel-tail", "page-cross"))
    add("wrap-4mib", F, 2, 200, 140, tbp=0x3fe0, tbw=5, tags=("synthetic", "wrap", "pages"))
    add("wrap-unaligned-tbp", F, 2, 64, 40, tbp=0x3fff, tbw=2, tags=("synthetic", "wrap"))
    add("high-origin", F, 2, 48, 40, x=2000, y=2000, tbp=0x500, tbw=32, tags=("synthetic", "dsax-dsay"))
    add("tbp-top", F, 2, 64, 64, tbp=0x3f00, tbw=1, tags=("synthetic", "wrap"))
    for pattern in (0, 1, 2, 3):
        add(f"preload-{PATTERN_NAMES[pattern]}", F, 2, 70, 30, x=3, y=2, tbp=0x60, tbw=2, pattern=pattern,
            pre_lo=0x6000 - 0x400, pre_len=0x5000, tags=("synthetic", "preload", "neighbours", "high-byte"))
    add("payload-all-00", F, 2, 64, 64, tbp=0x90, tbw=1, pattern=1, tags=("synthetic", "fill-equal-00"))
    add("payload-all-ff", F, 2, 64, 64, tbp=0x90, tbw=1, pattern=2, tags=("synthetic", "fill-equal-ff"))
    add("payload-f0-0f", F, 2, 64, 64, tbp=0x90, tbw=1, pattern=3, tags=("synthetic", "both-nibbles"))
    add("preload-wrap", F, 2, 128, 40, tbp=0x3ff0, tbw=2, pre_lo=0x3ff000, pre_len=0x1000,
        tags=("synthetic", "preload", "wrap", "high-byte"))

    def staged(name, pre_fmt, pw, ph, w, h, x=0, y=0, tbp=0x100, tbw=2, pattern=0, tags=()):
        add(name, F, 3, w, h, x=x, y=y, tbp=tbp, tbw=tbw, pattern=pattern, pre_lo=PSM[pre_fmt],
            pre_len=pw | (ph << 16), tags=("staged", f"pre-{pre_fmt}", "high-byte") + tuple(tags))

    staged("after-t4hl-same-rect", "t4hl", 64, 64, 64, 64, tbw=2, tags=("high-byte-lane2",))
    staged("after-t4hh-same-rect", "t4hh", 64, 64, 64, 64, tbw=2, tags=("high-byte-lane3",))
    staged("after-ct32-same-rect", "ct32", 64, 64, 64, 64, tbw=2, tags=("high-byte-known-word",))
    staged("after-t4hl-partial-overlap", "t4hl", 64, 64, 40, 20, x=5, y=3, tbw=2, tags=("partial-overlap",))
    staged("after-t4hh-offset-odd", "t4hh", 100, 40, 33, 17, x=7, y=9, tbw=2, tags=("partial-overlap", "dropped-pixel"))
    staged("after-ct32-partial-tail", "ct32", 40, 40, 40, 40, tbw=1, tags=("dropped-pixel",))
    staged("after-t4hl-tbw1-pages", "t4hl", 64, 140, 64, 140, tbw=1, tags=("pages",))
    staged("after-t4hh-wrap", "t4hh", 128, 40, 128, 40, tbp=0x3ff0, tbw=2, tags=("wrap",))
    staged("after-t8", "t8", 64, 64, 64, 64, tbw=1, tags=("t8-overlap",))
    staged("after-t4-swizzled", "t4", 64, 64, 64, 64, tbw=1, tags=("t4-overlap",))
    staged("after-t4hl-partial-pixel-tail", "t4hl", 8, 4, 6, 1, tbw=1, tags=("partial-pixel-tail",))
    staged("after-t4hh-all-ff", "t4hh", 64, 64, 64, 64, tbw=2, pattern=2, tags=("fill-equal-ff",))
    staged("after-t4hl-all-00", "t4hl", 64, 64, 64, 64, tbw=2, pattern=1, tags=("fill-equal-00",))

    # Kind 4: both allocated by unchanged 1cf008 (class 1), the 4-bit-high-lane image first, then PSM1.
    def pair(name, pre_fmt, pw, ph, w, h, tags=()):
        add(name, F, 4, w, h, pre_lo=PSM[pre_fmt], pre_len=pw | (ph << 16),
            tags=("allocated-pair", f"pre-{pre_fmt}", "high-byte") + tuple(tags))

    pair("pair-t4hl-64x64", "t4hl", 64, 64, 64, 64)
    pair("pair-t4hh-64x64", "t4hh", 64, 64, 64, 64)
    pair("pair-t4hl-33x17", "t4hl", 33, 17, 33, 17, tags=("dropped-pixel",))
    pair("pair-t4hh-100x30", "t4hh", 100, 30, 100, 30, tags=("odd-tail",))
    pair("pair-ct32-64x64", "ct32", 64, 64, 64, 64)
    pair("pair-t4hl-128x128", "t4hl", 128, 128, 128, 128, tags=("one-page",))
    pair("pair-t4hl-small-pre", "t4hl", 32, 32, 64, 64)
    pair("pair-t4hh-large-pre", "t4hh", 128, 64, 32, 32)

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
