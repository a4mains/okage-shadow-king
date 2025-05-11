#!/usr/bin/env python3
"""Generate and verify the Title logo/title construction source contract.

Window: from the first scheduler traversal (call at 0x100618) through the RETURN of
0x12c350(selector=1), before the first 0x1296f8 title update.  See
the title input contract.  Pure Python 3, standard library only.

  python3 tools/title_contract.py --local /path/to/okage/local --out docs/title-contract.json
  python3 tools/title_contract.py --check docs/title-contract.json

--local re-verifies every curated code/data anchor against the pinned ELF, re-reads the
owned SCR/NIGHT_V assets (sizes, formats, SHA256, the authored XMD name table) and
re-derives the ordered event list.  --check needs only the JSON: it replays the event
list through independent models of the handle/record/reservation/name-hash/image-list
state (the handle cache lifecycle contract) and the descriptor reference counts, and
fails on any inconsistency.  Nothing here executes game code.
"""
import argparse
import hashlib
import json
import os
import struct
import sys

ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
SCHEMA = 'okage.title-contract.v1'
TAGS = ('O', 'H', 'U')
HASH_SIZE = 0x400
NAME_CHARS = 19
HANDLE_IDS = 0x1000
RECORDS = 0x200
END_OF_LIST = -7          # 0x1cc778 at the in-use tail: node 1 (tail) - 8

class Elf:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        self.sha256 = hashlib.sha256(self.d).hexdigest()
        phoff = struct.unpack_from('<I', self.d, 0x1c)[0]
        n = struct.unpack_from('<H', self.d, 0x2c)[0]
        es = struct.unpack_from('<H', self.d, 0x2a)[0]
        self.segs = []
        for i in range(n):
            t, off, va, _pa, fs, ms, _fl, _al = struct.unpack_from('<8I', self.d, phoff + i * es)
            if t == 1:
                self.segs.append((va, off, fs, ms))

    def rb(self, va, n):
        for sva, off, fs, ms in self.segs:
            if sva <= va < sva + ms:
                o = va - sva
                out = self.d[off + o: off + min(o + n, fs)] if o < fs else b''
                return out + b'\0' * (n - len(out))
        raise ValueError(hex(va))

    def w32(self, va):
        return struct.unpack('<I', self.rb(va, 4))[0]

    def cstr(self, va):
        return self.rb(va, 256).split(b'\0')[0].decode('latin1')

    def branch_target(self, pc):
        w = self.w32(pc)
        op = w >> 26
        if op not in (2, 3):
            return None, None
        return ('jal' if op == 3 else 'j'), ((w & 0x3ffffff) << 2) | ((pc + 4) & 0xf0000000)

# Curated anchors: (pc, 'jal'|'j', target) re-verified against the ELF on every --local run.
CALL_ANCHORS = [
    (0x100618, 'jal', 0x114f50), (0x114f60, 'j', 0x114f70),
    (0x115024, 'jal', 0x114f70), (0x115078, 'jal', 0x114f70), (0x1150cc, 'jal', 0x1bdaf0), (0x1150e8, 'jal', 0x1bdaf0),
    (0x124974, 'j', 0x124930), (0x124878, 'jal', 0x1007e0), (0x1248a4, 'jal', 0x1246b0), (0x1248b0, 'jal', 0x1008d8),
    (0x1247d8, 'jal', 0x1151b8), (0x1151d4, 'jal', 0x114e98),
    (0x12c384, 'jal', 0x1151b8), (0x12c390, 'jal', 0x114c80), (0x12c3a0, 'jal', 0x100c58), (0x12c3b4, 'jal', 0x1cb4b8),
    (0x12c3bc, 'jal', 0x12d470), (0x12c3c4, 'jal', 0x1c0930), (0x12c3d0, 'jal', 0x1bccf0),
    (0x12c408, 'jal', 0x100a60), (0x12c418, 'jal', 0x100a60),
    (0x12c430, 'jal', 0x100a60), (0x12c440, 'jal', 0x100a60), (0x12c450, 'jal', 0x100a60), (0x12c460, 'jal', 0x100a60),
    (0x12c470, 'jal', 0x100a60), (0x12c480, 'jal', 0x100a60), (0x12c490, 'jal', 0x100a60), (0x12c4a0, 'jal', 0x100a60),
    (0x12c4a8, 'jal', 0x1b36c0), (0x12c4d4, 'jal', 0x16c3c8), (0x12c50c, 'jal', 0x1bcc80), (0x12c514, 'jal', 0x1c0930),
    (0x12c788, 'jal', 0x1c0930), (0x12c794, 'jal', 0x1bccf0), (0x12c7a0, 'jal', 0x186c48), (0x12c7bc, 'jal', 0x1b8a80),
    (0x12c7cc, 'jal', 0x1bb728), (0x12c7d4, 'jal', 0x1bcc80), (0x12c7dc, 'jal', 0x1c0930), (0x12c86c, 'jal', 0x12db50),
    (0x12c884, 'jal', 0x12dea0), (0x12c894, 'jal', 0x114cc0), (0x12c8b4, 'j', 0x114c80),
    (0x12c8e8, 'j', 0x12dd50), (0x12c8f8, 'j', 0x12bfc8),
    (0x12c22c, 'j', 0x129570), (0x12c25c, 'j', 0x1296f8), (0x12c2bc, 'j', 0x12b508), (0x12bae8, 'jal', 0x124930),
    (0x1295d4, 'jal', 0x12cde8), (0x1295ec, 'jal', 0x1bc538), (0x12960c, 'jal', 0x1bc538), (0x129618, 'jal', 0x1bc538),
    (0x129624, 'jal', 0x1bc538), (0x129630, 'jal', 0x1bc538), (0x12963c, 'jal', 0x1bc538), (0x1296a0, 'jal', 0x124930),
    (0x1296e0, 'j', 0x1b3710),
    (0x100a68, 'jal', 0x1009e0), (0x100a04, 'jal', 0x1b2348), (0x100a1c, 'jal', 0x1b26a0), (0x1b26ac, 'jal', 0x1b25a8),
    (0x1b2358, 'jal', 0x1c2e68), (0x1b239c, 'jal', 0x18f7a8), (0x18f7ec, 'jal', 0x1cd1e0), (0x18f82c, 'jal', 0x1cd7d0),
    (0x18f88c, 'jal', 0x1cd298), (0x1cd20c, 'jal', 0x1cf008), (0x1cd220, 'jal', 0x1cc880), (0x1cd274, 'jal', 0x1cba48),
    (0x1cd810, 'jal', 0x1cf008), (0x1cd9a8, 'jal', 0x1cba48),
    (0x100fb0, 'jal', 0x100800), (0x100fcc, 'jal', 0x1b9be0), (0x101000, 'jal', 0x114760),
    (0x10107c, 'jal', 0x1b5fc8), (0x101090, 'jal', 0x1891b8), (0x101098, 'jal', 0x114530), (0x1010a0, 'jal', 0x101240),
    (0x1010a8, 'jal', 0x198550), (0x1010bc, 'jal', 0x1cca30), (0x1010c4, 'jal', 0x13c368), (0x1010cc, 'jal', 0x1b84b0),
    (0x1010d4, 'jal', 0x1b6f50), (0x1010dc, 'jal', 0x1b5fd0), (0x1010e4, 'jal', 0x1b3fc0), (0x1010ec, 'jal', 0x18fb00),
    (0x1010f4, 'jal', 0x1b8028), (0x10ce94, 'jal', 0x1cca30),
    (0x1b6070, 'jal', 0x1b4b00), (0x1b4b00, 'j', 0x1b2240), (0x1b227c, 'jal', 0x1cca30),
    (0x186c54, 'jal', 0x1c2e90), (0x186c68, 'jal', 0x186aa8), (0x186c7c, 'jal', 0x183450),
    (0x186b00, 'jal', 0x1cd5c8), (0x186ba4, 'jal', 0x1cce78), (0x186bb8, 'jal', 0x1c2e68), (0x186be0, 'jal', 0x1cc4e0),
    (0x18346c, 'jal', 0x179478), (0x183480, 'jal', 0x182f08), (0x182f60, 'jal', 0x1cfc00), (0x182ffc, 'jal', 0x182e28),
    (0x182e78, 'jal', 0x186950), (0x182eac, 'jal', 0x1cc778),
    (0x17bef0, 'jal', 0x1cd500), (0x17be38, 'jal', 0x1cc2b0), (0x17beb8, 'jal', 0x17be28),
    (0x178be8, 'jal', 0x178910), (0x178c2c, 'j', 0x178910), (0x178bfc, 'jal', 0x1e43b0), (0x178c08, 'jal', 0x1e4460),
    (0x178c10, 'jal', 0x1e4390), (0x1bbac8, 'jal', 0x186a50),
    (0x10068c, 'jal', 0x1cb648), (0x100698, 'jal', 0x1cad20), (0x100628, 'jal', 0x18f9f8), (0x100638, 'jal', 0x100228),
    (0x1cb738, 'jal', 0x1cb580), (0x1cb614, 'jal', 0x1cad20), (0x100244, 'jal', 0x197a98), (0x114d08, 'jal', 0x18fbf0),
]
# Data-word anchors (address, expected u32).
WORD_ANCHORS = [
    (0x223790, 0x124940), (0x223798, 0x124988), (0x22379c, 0x124980),
    (0x223760 + 4 * 7, 0x1247b0),                                   # mode table: mode 8 -> 0x1247b0
    (0x227988, 0x12c350), (0x22798c, 0), (0x227990, 0x12c8f8), (0x227994, 0x12c8d8),
    (0x221980, 0x100ef8), (0x221984, 0), (0x221988, 0x100f90), (0x22198c, 0x101058),
    (0x227650, 0x12c204), (0x227654, 0x12c234), (0x22765c, 0x12c294),   # mode8 update switch 0/1/3
    (0x227970, 0x12c400), (0x227974, 0x12c428), (0x22797c, 0x12c4b8),   # constructor switch 0/1/3
    (0x1fe308, 0x18f7a8), (0x1fe30c, 0), (0x1fe310, 0),                 # loader, lower-case flag, cache flag
    (0x1fe244, 0), (0x1fe248, 0),                                       # worker job ring head/tail (empty)
    (0x2003b0 + 4 * 2, 0x1942f0), (0x2003b0 + 4 * 6, 0x192e08), (0x2003b0 + 4 * 7, 0x192e70), (0x2003b0 + 4 * 8, 0x192e70),
]
STRING_ANCHORS = {
    0x2273e8: 'cmndata/2d/', 0x227668: 'scr.xpf', 0x227670: 'scea.xim', 0x227680: 'zener.xim',
    0x227690: 'title_sample4.xim', 0x2276a8: 'push.xim', 0x2276b8: 'new.xim', 0x2276c0: 'restore.xim',
    0x2276d0: 'maru_w.xim', 0x2276e0: 'maru_b.xim', 0x2276f0: 'sp.xim', 0x2276f8: 'hanten.xim',
    0x227928: 'night_v.xpf', 0x227938: 'a.xmd', 0x227958: 'snddata/str/thema_1', 0x1fe300: 'xim',
    0x227250: 'flddata/event/', 0x227260: 'evdata.xpf', 0x227270: 'flddata/obj/', 0x227280: 'hero.xpf',
    0x227290: 'stun.xpf', 0x2272a0: 'rkumo.xpf', 0x2272b0: 'wkumo.xpf', 0x2272c0: 'bkumo.xpf',
}
LOGO_SITES = [(0x12c408, 0x227670), (0x12c418, 0x227680)]
TITLE_SITES = [(0x12c430, 0x227690), (0x12c440, 0x2276a8), (0x12c450, 0x2276b8), (0x12c460, 0x2276c0),
               (0x12c470, 0x2276d0), (0x12c480, 0x2276e0), (0x12c490, 0x2276f0), (0x12c4a0, 0x2276f8)]

def lui_addiu_arg(elf, pc):
    """a1 constant set by the 'lui a1,X' ... delay-slot 'addiu a1,a1,Y' pattern of a 100a60 call at pc."""
    lui = elf.w32(pc - 8)
    add = elf.w32(pc + 4)
    if lui >> 26 != 0x0f or add >> 26 != 0x09:
        raise ValueError('argument pattern at %x' % pc)
    lo = add & 0xffff
    lo = lo - 0x10000 if lo & 0x8000 else lo
    return ((lui & 0xffff) << 16) + lo

def verify_elf(elf):
    bad = []
    if elf.sha256 != ELF_SHA256:
        bad.append('ELF sha256')
    for pc, kind, target in CALL_ANCHORS:
        k, t = elf.branch_target(pc)
        if (k, t) != (kind, target):
            bad.append('call anchor %x: %s %s' % (pc, k, hex(t) if t else t))
    for a, v in WORD_ANCHORS:
        if elf.w32(a) != v:
            bad.append('word anchor %x = %x' % (a, elf.w32(a)))
    for a, s in STRING_ANCHORS.items():
        if elf.cstr(a) != s:
            bad.append('string anchor %x = %r' % (a, elf.cstr(a)))
    for pc, s in LOGO_SITES + TITLE_SITES:
        if lui_addiu_arg(elf, pc) != s:
            bad.append('image name argument at %x' % pc)
    return bad

def sha(b):
    return hashlib.sha256(b).hexdigest()

def parse_xim(b):
    """0x18f6a8: psm=(u64@0>>20)&0x3f, cpsm=(u64@0>>51)&0xf.  psm<0x10: no palette block and the
    image block is at +0x10; otherwise palette block at +0x10 (+0 extent, +8 levels, +0xc colours,
    entries at +0x20).  Image block: +8 height, +0xc width, pixels at +0x10."""
    q0 = struct.unpack_from('<Q', b, 0)[0]
    psm = (q0 >> 20) & 0x3f
    cpsm = (q0 >> 51) & 0xf
    if psm < 0x10:
        ext, levels, colors = 0, 0, 0
    else:
        ext = struct.unpack_from('<i', b, 0x10)[0]
        levels, colors = struct.unpack_from('<2I', b, 0x18)
    ib = 0x10 + ext
    height, width = struct.unpack_from('<2I', b, ib + 8)
    q = qwc(psm, width, height)
    pix = b[ib + 0x10: ib + 0x10 + 16 * q]
    pal = [struct.unpack_from('<I', b, 0x20 + 4 * i)[0] for i in range(colors * max(levels, 0))]
    return dict(psm=psm, cpsm=cpsm, levels=levels, colors=colors, width=width, height=height,
                pixel_offset=ib + 0x10, qwc=q, pixels=pix, palette=pal, size=len(b))

def qwc(psm, w, h):
    """0x1c9b48 jump table (0x24b7c0): quadwords sent by 0x1c9bb8 (floor)."""
    n = w * h
    if psm in (0x00, 0x30):
        return n >> 2
    if psm in (0x01, 0x31):
        return (n * 3) >> 4            # 0x1c9b7c..0x1c9b8c
    if psm in (0x02, 0x0a, 0x32, 0x3a):
        return n >> 3
    if psm in (0x13, 0x1b):
        return n >> 4
    if psm in (0x14, 0x24, 0x2c):
        return n >> 5
    raise ValueError('psm %d' % psm)

def clut_rows(colors):
    """0x1cd7d0: class-0 PSM0 allocation 16 x ((n+31)>>5)<<1."""
    return ((colors + 31) >> 5) << 1

def clut_swizzle(words, n):
    """1cd858 + 1cd928: zero 512-word buffer, entry i -> CSM1 position."""
    buf = [0] * 512
    for i in range(n):
        pos = (((i >> 5) << 1) + ((i & 8) >> 3)) * 16 + (((i & 0x10) >> 4) << 3) + (i & 7)
        buf[pos] = words[i]
    return buf

def clut_upload_bytes(words, n):
    rows = clut_rows(n)
    buf = clut_swizzle(words, n)
    return b''.join(struct.pack('<I', x) for x in buf[:16 * rows]), rows

def xmd_names(b):
    """0x186950 / 0x186978: off=u32@4 (<=0: none); count=u32@off; NUL-terminated names after off+4."""
    off = struct.unpack_from('<i', b, 4)[0]
    if off <= 0:
        return []
    n = struct.unpack_from('<I', b, off)[0]
    p, out = off + 4, []
    for _ in range(n):
        e = b.index(b'\0', p)
        out.append(b[p:e].decode('latin1'))
        p = e + 1
    return out

def stored_name(file_name):
    """0x1cc480: lower-case ASCII A-Z, at most 19 characters."""
    return ''.join(chr(ord(c) + 32) if 'A' <= c <= 'Z' else c for c in file_name)[:NAME_CHARS]

def hash_probe(name):
    s = sum(name.encode('latin1'))
    return s % HASH_SIZE, ((((~s) & 0xf) << 1) + 1)

class NameHash:
    """0x1cc4e0 registration (hash 0x3a7b0c): 0 empty, -1 tombstone, r+8 occupied; newest duplicate wins."""

    def __init__(self):
        self.table = [0] * HASH_SIZE
        self.slot = {}       # record -> slot
        self.name = {}       # record -> stored name

    def register(self, record, file_name):
        nm = stored_name(file_name)
        old = self.slot.get(record, -1)
        if old >= 0:
            self.table[old] = -1
        self.slot[record] = -1
        self.name[record] = nm
        i, step = hash_probe(nm)
        first_free, found = -1, None
        while True:
            e = self.table[i]
            if e > 0:
                if self.name.get(e - 8) == nm:
                    found = e
                    break
            else:
                if first_free < 0:
                    first_free = i
                if e == 0:
                    break
            i = (i + step) % HASH_SIZE
        displaced = None
        if found is not None:
            slot = self.slot[found - 8]
            self.slot[found - 8] = -1
            displaced = found - 8
        else:
            slot = first_free
        self.slot[record] = slot
        self.table[slot] = record + 8
        return slot, displaced

class HandleModel:
    """Logical handle/record/reservation state (the handle cache lifecycle contract)."""

    def __init__(self, live_in_order, records_in_order):
        self.used = set(live_in_order)                 # HR allocated ids
        self.in_use = list(live_in_order)              # H list A order
        self.map = dict(zip(live_in_order, records_in_order))
        self.rec_handles = {r: 1 for r in records_in_order}
        used_r = set(records_in_order)
        self.free_records = [r for r in range(RECORDS) if r not in used_r]   # R list D (FIFO)
        self.queue = {0: [], 1: []}                    # reserved queues B (cls 0) and C (cls 1)

    def _hr_alloc(self, n):
        i = 0
        while i + n <= HANDLE_IDS:
            if all((i + k) not in self.used for k in range(n)):
                for k in range(n):
                    self.used.add(i + k)
                return i
            i += 1
        return -1

    def create(self, cls=0):
        if not self.free_records:
            return -1, -1
        q = self.queue[cls]
        if q:
            h = q.pop(0)
        else:
            h = self._hr_alloc(1)
            if h < 0:
                return -1, -1
        r = self.free_records.pop(0)
        self.map[h] = r
        self.rec_handles[r] = 1
        self.in_use.append(h)
        return h, r

    def reserve(self, n, cls=0):
        for h in self.queue[cls]:
            self.used.discard(h)
        self.queue[cls] = []
        s = self._hr_alloc(n)
        if s < 0:
            return -1
        self.queue[cls] = list(range(s, s + n))
        return s

    def retire(self, h):
        r = self.map.pop(h)
        self.rec_handles[r] -= 1
        last = self.rec_handles[r] == 0
        if last:
            del self.rec_handles[r]
            self.free_records.append(r)
        self.in_use.remove(h)
        self.used.discard(h)
        return r, last

    def successor(self, h):
        """0x1cc778: next live handle in H list A, END_OF_LIST at the tail, -1 for invalid."""
        if h < 0 or h not in self.map:
            return -1
        i = self.in_use.index(h)
        return self.in_use[i + 1] if i + 1 < len(self.in_use) else END_OF_LIST

def xmd_binding(model, first, count):
    """0x182e28: if first<0 the table is empty; else handles[i]=h; h=0x1cc778(h) for i<count."""
    if first < 0:
        return []
    out, h = [], first
    for _ in range(count):
        out.append(h)
        h = model.successor(h)
    return out

MAIN_LOOP = [
    ('0x1005e0', '0x114b58', 'fade stepping (0x114ac0 slot-1 overlay, slot-2 fade); disables slot 2 via 0x18fbf8 when done'),
    ('0x1005e8', '0x1bfb98', 'pad/input'), ('0x1005f0', '0x197518', 'font cache reset'),
    ('0x1005f8', '0x1c8ed0', 'DMA pool'), ('0x100600', '0x1c8f50', 'frame display-list buffer 0x4140 -> *0x1f8480'),
    ('0x100610', '0x1c9420', 'list init'), ('0x100618', '0x114f50', 'SCHEDULER TRAVERSAL'),
    ('0x100628', '0x18f9f8', 'filter dispatcher: enabled slots of 0x332934 via callbacks 0x2003b0'),
    ('0x100630', '0x1000c8', 'text list'), ('0x100638', '0x100228', 'glyph-cache transfer list (0x197a98)'),
    ('0x100640', '0x1cff30', 'vsync count'), ('0x100650', '0x178310', 'vsync wait: enters 0x178b50 owner window'),
    ('0x10066c', '0x179388', 'vsync wait: enters 0x178b50 owner window'), ('0x100684', '0x1cbbf8', 'PMODE'),
    ('0x10068c', '0x1cb648', 'swap layers; 0x1cb580 kicks the draw environment + clear sprite (0x1cad20 at 0x1cb614)'),
    ('0x100698', '0x1cad20', 'kick the frame display list *0x1f8480'), ('0x1006a0', '0x12ce20', 'sound'),
    ('0x1006bc', '0x125330', 'every 60 iterations'),
]

GS_WRITERS = [
    {'id': 'W1', 'kind': 'clear', 'pcs': ['0x10068c', '0x1cb648', '0x1cb580', '0x1cb614', '0x1e09e8'], 'tag': 'O',
     'executes': 'every main-loop iteration in the window (including the iteration of the first traversal)',
     'what': 'draw environment + sceGsSetDefClear-style sprite (TEST ZTE/ALWAYS, PRIM 6, RGBAQ = layer clear colour '
             'bytes +0xc0..0xc3 set by 0x1cb4b8, two XYZ2) built by 0x1e09e8 and kicked by 0x1cb580',
     'regions': [{'alloc': 'A00', 'lanes': 'RGBA32 640x224', 'status': 'invalidated'},
                 {'alloc': 'A01', 'lanes': 'RGBA32 640x224', 'status': 'invalidated'},
                 {'alloc': 'A02', 'lanes': 'Z24 (byte lanes 0..2 of each word)', 'status': 'invalidated',
                  'note': 'PSMZ24 writes never touch byte lane 3; the font PSM36/44 nibbles sharing lane 3 '
                          '(Startup errata 3) are not written by this writer [H: ZBUF.ZMSK and exact env qword count '
                          'not decoded]'}]},
    {'id': 'W2', 'kind': 'frame-draw', 'pcs': ['0x100698', '0x1cad20'], 'tag': 'O',
     'executes': 'every main-loop iteration',
     'what': 'frame list *0x1f8480 assembled by the traversal and main loop: resource tick 0x100f90 (0x1b9be0 3D pass, '
             '0x100d40, 0x1012a8, image sprites 0x100800->0x1b5ee0->0x1b3560->0x1b2e38->0x1b29b8 when resource+0x24), '
             'fonts 0x1980f8/0x198390/0x1000c8, filter slot 2 fade 0x1942f0->0x193c20 (FRAME/ZBUF of the current layer, '
             'TEST 0x3000c)',
     'regions': [{'alloc': 'A00', 'lanes': 'RGBA32', 'status': 'invalidated'},
                 {'alloc': 'A01', 'lanes': 'RGBA32', 'status': 'invalidated'},
                 {'alloc': 'A02', 'lanes': 'Z24 byte lanes 0..2', 'status': 'invalidated'}]},
    {'id': 'W3', 'kind': 'host-to-local (packet IMAGE, not 0x1cba48)', 'tag': 'U',
     'pcs': ['0x100238', '0x197a98', '0x197b28', '0x197c68', '0x197eac', '0x1976f0', '0x197874', '0x1978ec', '0x197910'],
     'executes': 'only when glyphs are queued for the dynamic glyph cache; 0x100228 runs every iteration',
     'what': 'BITBLTBUF(dst = descriptor of handle *0x200494 = glyph cache) / TRXPOS / TRXREG / TRXDIR=0 + IMAGE '
             'GIFtag, glyph pixels copied from the font atlas',
     'regions': [{'alloc': 'A03', 'lanes': 'PSMT4 glyph rectangles', 'status': 'unknown-if-text-drawn'}]},
    {'id': 'W4', 'kind': 'host-to-local (0x1afb48->0x1c9bb8)', 'tag': 'O', 'pcs': ['0x101000', '0x114760', '0x1147e0', '0x114864'],
     'executes': 'never in the window: guarded by aux texture handles 0x24ea94/0x24ea90 = -1 (0x1145a0 in the prefix); '
                 'their only setter 0x1145c0 is not reachable from the window closure',
     'regions': []},
    {'id': 'W5', 'kind': 'filter capture / CLUT fill (slots 6,7,8)', 'tag': 'O',
     'pcs': ['0x18f928', '0x192e08', '0x192e70', '0x1902d8'],
     'executes': 'never in the window: 0x18fb00 and the startup 0x18faa0 clear the slots; slot 6/7/8 setters are 0x1bb088 and '
                 '0x1b7ec0, reached only from 0x1b9be0 when 0x337e18 != 0, and 0x337e18 is set only by 0x1bb858 (not '
                 'reachable; reset by 0x1b8028)',
     'regions': [{'alloc': 'A08', 'lanes': 'all', 'status': 'unwritten'},
                 {'alloc': 'A09', 'lanes': 'all', 'status': 'unwritten'},
                 {'alloc': 'A10', 'lanes': 'all', 'status': 'unwritten'}]},
    {'id': 'W6', 'kind': 'host-to-local 0x1cba48->0x1c9bb8 (modeled exactly)', 'tag': 'O',
     'pcs': ['0x1cd274', '0x1cd9a8'], 'executes': '18f7a8 image/CLUT uploads in the event list',
     'regions': [{'alloc': 'events', 'lanes': 'upload rectangles', 'status': 'known-bytes'}]},
]
NOT_GS_WRITERS = [
    'No TRXDIR=2 (local-to-local) or other BITBLTBUF/TRXDIR writer is in the window closure besides 0x1c9bb8/0x1c9e38/'
    '0x1ca160 (host-to-local helpers; 0x1c9e38/0x1ca160 never called in the window) and 0x1976f0 [O: immediate scan of the '
    'window closure for A+D register ids 0x50..0x53; 0x1b29b8 uses 0x52/0x53 as GIF REGS descriptors, not registers].',
    'TEX0/CLD CLUT loads write the GS CLUT buffer, not local memory.',
    'Display (CRTC) reads A00/A01 only; VU1/VIF microprogram packets do not address GS local memory.',
]

PARTIAL_FAILURES = [
    {'site': '0x1b2348', 'tag': 'O', 'case': '1c2e68(name) returns 0',
     'effect': 'log via 0x1c09c8/0x1c7910, return -1; NOTHING appended to 0x337508'},
    {'site': '0x1b2348', 'tag': 'O', 'case': '18f7a8 returns -1 (1cd1e0 allocation or handle failure)',
     'effect': 'file buffer freed; -1 IS appended to 0x337508[count++] and returned; 0x1b2240 later calls 1cca30(-1) = no-op'},
    {'site': '0x18f7a8/0x1cd1e0', 'tag': 'O', 'case': '1cf008 fails', 'effect': '-1, no handle, no upload'},
    {'site': '0x18f7a8/0x1cd1e0', 'tag': 'O', 'case': '1cc880 fails',
     'effect': '1cf728(desc) frees the fresh zero-reference descriptor; -1'},
    {'site': '0x18f7a8', 'tag': 'O', 'case': '1cd7d0 palette allocation fails',
     'effect': 'null descriptor bound at that level; load still returns the handle'},
    {'site': '0x18f7a8/0x1cd298', 'tag': 'O', 'case': 'extra-palette pool exhausted (level>=1)',
     'effect': 'not reachable here: every reached XIM has levels <= 1'},
    {'site': '0x1009e0', 'tag': 'O', 'case': '1b2348 < 0',
     'effect': 'returns -1; no 1b26a0 image record, resource +0x24/+0x2c/+0x30 unchanged. After an 18f7a8 failure the '
               'list count (0x337608) exceeds resource+0x30, so the next successful image record maps to list[+0x30] = the '
               'failed -1 entry, not its own handle'},
    {'site': '0x12c4a8', 'tag': 'O', 'case': 'hanten 100a60 returned -1',
     'effect': '1b36c0(-1) reads image record -1 (*0x33749c - 0x30): out-of-table read stored at mode8+0xcc'},
    {'site': '0x186c48', 'tag': 'O', 'case': '1c2e90("a.xmd") returns 0', 'effect': 'returns -1 without 186aa8'},
    {'site': '0x186aa8', 'tag': 'O', 'case': 'name count <= 0',
     'effect': 'returns 0 without reservation; 186c48 then calls 183450(xmd, 0): binding count = 0 (no handles)'},
    {'site': '0x186aa8', 'tag': 'O', 'case': '1cd5c8 fails', 'effect': '-1; queue-B releases of phase 1 persist'},
    {'site': '0x186aa8', 'tag': 'O', 'case': '1c2e68(name.xim) returns 0',
     'effect': 'return -1 immediately: earlier handles, names and unused reservations stay; no free of that buffer'},
    {'site': '0x186aa8', 'tag': 'O', 'case': '18f7a8 returns -1 for a name',
     'effect': 'loop continues; 1cc4e0(-1) no-op; no handle consumed for that name, so the reserved queue is consumed by '
               'later names and every later successor slot shifts'},
    {'site': '0x182f08', 'tag': 'O', 'case': 'mesh-slot range 1cfc00(*0x330140, meshes) fails',
     'effect': 'returns -1 before 182e28 (no binding record); loaded handles stay live'},
    {'site': '0x12c7a8', 'tag': 'O', 'case': '186c48 < 0',
     'effect': '12c350 returns at 0x12c8bc: skips 1b8a80/1bb728, the night_v.xpf unmount 1bcc80 and 1c0930(0), mode8+0x2c/'
               '+0x30/+0xb8, the 0x25990c.. stores, 12db50/12dea0 theme, 114cc0(1.0) and 114c80(0)'},
    {'site': '0x182e28', 'tag': 'O', 'case': 'fewer live successors than names',
     'effect': 'handles[i] continues past the last created handle: END_OF_LIST (-7) once, then -1 (1cc778(-7) = -1)'},
    {'site': '0x17bda8', 'tag': 'O', 'case': 'material texture index t<0 or t >= binding count',
     'effect': 'returns handle 0 (the glyph-cache handle), logged when debug output is on'},
    {'site': '0x17be28/0x17be50', 'tag': 'O', 'case': 'binding entry -1 or -7',
     'effect': '1cc2b0/1cd500 return descriptor 0 (TEX0 built from a null descriptor)'},
]

UNRESOLVED = [
    ('U01', 'Allocator-derived values (base block, TBW, free-list reuse after the four logo frees) are NOT read from the '
            'ELF; 87 new allocations and 4 frees must be replayed by the unchanged original 0x1cf008/0x1cf728 (Startup U01).'),
    ('U02', 'Indirect calls are not resolved: device dispatch 0x1c4df0/0x1c4da0 under 0x1c2e68, allocator slots '
            '0x213574..0x213580, the 0x178910 callback table, scheduler constructor/callback jalr, filter callbacks '
            '0x2003b0 and the 0x1fe308 loader (=0x18f7a8 by ELF data). Every "not reached" statement is modulo these.'),
    ('U03', 'Storage readiness is asynchronous (card thread 0x16d918 / poll 0x16c2a0); the number of selector-3 ticks is '
            'not static. The contract fixes event order, not frame numbers.'),
    ('U04', 'GS register contents of the per-frame clear and draw packets (ZBUF.ZMSK, FBMSK, SCISSOR, env qword count) '
            'are not decoded; A00/A01 and A02 Z24 lanes are therefore conservatively invalidated, not modeled.'),
    ('U05', 'Glyph-cache transfers (0x197a98 -> 0x1976f0) happen only if text glyphs are queued; static reading does not '
            'prove no text is queued in storage/logo frames (12b508 -> 129388 message path, debug print paths).'),
    ('U06', 'Handle ids 36..43/44..110 and record indices 36..112 are derived with the Handle first-fit / FIFO semantics '
            'and the reservation order, not observed (Startup U03 continued).'),
    ('U07', 'Model instance table *0x21301c and model count 0x337ef0 are assumed inactive at both retirements: their only '
            'activator 0x1b8a80 is first reached at 0x12c7bc (after both retirements); writes through the heap pointer '
            'were not enumerated exhaustively.'),
    ('U08', 'Archive member lookup is case-insensitive (0x1bc700 lower-cases the query in place); that 0x1c2e68 reaches '
            '0x1bc700 through the device dispatch jalr is inferred, so the five case-variant duplicate names resolve to '
            'the same member.'),
    ('U09', 'The material table 0x3301a4 (0x18-byte records: s16 texture, s16 palette level, s16 u/v modes, 4 floats) is '
            'read by 0x17be50 with this layout; that 0x182c90.. fill it from the XMD TextureState records is inferred.'),
    ('U10', 'The two startup embedded-model binding records (0x18e4c0 -> 183450(xmd, first=0), bound to handle 0 and its '
            'successors) stay in the 0x330144 list; NIGHT_V becomes the head record. Their exact name counts were not '
            'decoded here (outside the window).'),
    ('U11', 'RNG beyond 0x100618: 0x105a40 (rand at 0x10a558) is reachable from resource tick 0x1012a8; with no effect '
            'instance live in this window no draw is expected, but this is not proved.'),
    ('U12', 'Owner-window (0x178b50) entries during this window: per-frame 0x178310/0x179388, contended 0x179478 '
            '(0x183450) and allocator semaphore 0x1ce8c8 (0x1cf728). The worker thread then runs queued jobs with cls=1; '
            'no job post (0x178578) is reachable, so the queue stays empty (ELF 0x1fe244 == 0x1fe248 == 0).'),
]

def build_initial_state(startup):
    live = {}
    handles = []
    records = []
    for e in startup['events']:
        if e['kind'] == 'handle_create':
            handles.append(e['handle'])
            records.append(e['record'])
    for aid, refs in startup['final_references'].items():
        live[aid] = refs
    return {'live_allocations': live, 'live_handles_in_order': handles, 'handle_records': records,
            'reserved_queues': {'0': [], '1': []}, 'name_hash_entries': 0, 'image_list_count': 0,
            'cache_enabled': 0, 'lower_case_file_names': 0, 'reserved_class': 0}

class Builder:
    def __init__(self, initial):
        self.events = []
        self.next_alloc = len(initial['live_allocations'])
        self.refs = dict(initial['live_allocations'])
        self.model = HandleModel(initial['live_handles_in_order'], initial['handle_records'])
        self.names = NameHash()
        self.image_list = []
        self.counts = dict(allocations=0, uploads=0, handles_created=0, palette_binds=0, handles_retired=0,
                           records_destroyed=0, frees=0, reservations=0, names_registered=0, bindings=0,
                           image_list_appends=0)
        self.handle_alloc = {}
        self.handle_pal = {}

    def ev(self, kind, tag, pc, **kw):
        e = {'seq': len(self.events), 'kind': kind, 'tag': tag, 'pc': pc}
        e.update(kw)
        self.events.append(e)
        return e

    def alloc(self, cls, psm, w, h, pc, via, purpose):
        aid = 'A%02d' % self.next_alloc
        self.next_alloc += 1
        self.refs[aid] = 0
        self.counts['allocations'] += 1
        self.ev('alloc', 'O', pc, id=aid, alloc_class=cls, psm=psm, width=w, height=h, via=via,
                purpose=purpose, initial_references=0)
        return aid

    def ref(self, aid, delta, pc, reason):
        self.refs[aid] += delta
        self.ev('ref_adjust', 'O', pc, alloc=aid, delta=delta, references_after=self.refs[aid], reason=reason)

    def load_xim(self, b, name, phase):
        """0x18f7a8 on owned bytes: 1cd1e0 (alloc, handle, upload) then per level 1cd7d0 + 1cd298."""
        x = parse_xim(b)
        img = self.alloc(1, x['psm'], x['width'], x['height'], '0x1cd20c', '0x18f7ec -> 0x1cd1e0', name + ' image')
        h, r = self.model.create(0)
        self.counts['handles_created'] += 1
        self.handle_alloc[h] = img
        self.ev('handle_create', 'H', '0x1cd220', handle=h, record=r, image_alloc=img, reserved_class=0,
                from_reserved_queue=phase == 'night_v', name=name,
                id_derivation='Handle first-fit HR / FIFO record list; reserved queue B head when reserved')
        self.ref(img, +1, '0x1cc9dc', 'record image reference (1cc880)')
        self.counts['uploads'] += 1
        self.ev('upload', 'O', '0x1cd274', alloc=img, psm=x['psm'], x=0, y=0, rrw=x['width'], rrh=x['height'],
                qwc=x['qwc'], bytes=16 * x['qwc'], sha256=sha(x['pixels']), transfer='0x1cba48 -> 0x1c9bb8',
                source='%s +0x%x' % (name, x['pixel_offset']))
        pals = []
        for lvl in range(x['levels']):
            words = x['palette'][lvl * x['colors']:(lvl + 1) * x['colors']]
            data, rows = clut_upload_bytes(words, x['colors'])
            pal = self.alloc(0, 0, 16, rows, '0x1cd810', '0x18f82c -> 0x1cd7d0', name + ' CLUT level %d' % lvl)
            self.counts['uploads'] += 1
            self.ev('upload', 'O', '0x1cd9a8', alloc=pal, psm=0, x=0, y=0, rrw=16, rrh=rows, qwc=rows * 4,
                    bytes=len(data), sha256=sha(data), transfer='0x1cd928 -> 0x1cba48 -> 0x1c9bb8',
                    source='%s palette level %d swizzled by 0x1cd858' % (name, lvl))
            self.counts['palette_binds'] += 1
            self.ev('palette_bind', 'O', '0x18f88c', handle=h, level=lvl, alloc=pal)
            self.ref(pal, +1, '0x1cd318', 'palette level %d bind (1cd298)' % lvl)
            pals.append(pal)
        self.handle_pal[h] = pals
        return h, r, x

    def retire(self, h, pc):
        r, last = self.model.retire(h)
        self.counts['handles_retired'] += 1
        self.ev('handle_retire', 'H', pc, handle=h, record=r, last_handle=last)
        if last:
            img = self.handle_alloc.pop(h)
            self.ref(img, -1, '0x1ccbd8', 'record image release (1cca30 step 3)')
            if self.refs[img] < 1:
                self.counts['frees'] += 1
                self.ev('free', 'O', '0x1ccbe0', alloc=img, via='0x1cf728')
            for pal in self.handle_pal.pop(h):
                self.ref(pal, -1, '0x1ccbf8', 'level-0 palette release (1cca30 step 4)')
                if self.refs[pal] < 1:
                    self.counts['frees'] += 1
                    self.ev('free', 'O', '0x1ccc00', alloc=pal, via='0x1cf728')
            self.counts['records_destroyed'] += 1
            self.ev('record_release', 'H', '0x1ccc10', record=r, note='record node -> R free-list tail (fields left stale)')
        self.ev('handle_id_free', 'H', '0x1cccd0', handle=h, note='H node -> D tail, map[h]=-1, HR.free(h), live--')

def resource_dtor_events(B, selector, retire_list):
    E = B.ev
    E('destructor', 'O', '0x11509c', node='resource 0x221980', dtor='0x101058', selector=selector,
      order='descendant first: resource child (0x101058) then mode8 node (0x12c8d8) in the same traversal')
    E('call', 'O', '0x101074', callee='0x1b5fc8', taken=False, reason='resource+0x20 == -1 (0x100ef8)')
    E('call', 'O', '0x101088', callee='0x1891b8', taken=False, reason='resource+0x34 == 0')
    E('call', 'O', '0x101098', callee='0x114530', taken=True, resource='none: both aux texture ids are -1')
    E('call', 'O', '0x1010a0', callee='0x101240', taken=True,
      resource='0x1036d8 count; 0x10d150 host loop; 0x10ce58 retires 0x24e9a8[0..*0x24e9f8): count 0 (only writer 0x10cdf0 not reachable)')
    E('call', 'O', '0x1010a8', callee='0x198550', taken=True, resource='none (closure)')
    E('call', 'O', '0x1010b0', callee='0x1cca30', taken=False, reason='resource+0x48 == -1 (0x100ef8)')
    E('call', 'O', '0x1010c4', callee='0x13c368', taken=True, resource='none (closure)')
    E('call', 'H', '0x1010cc', callee='0x1b84b0', taken=True,
      resource='no model (0x337ef0 == 0) and no active instance in *0x21301c: no 0x1797b8/0x179650 retire (U07)')
    E('call', 'O', '0x1010d4', callee='0x1b6f50', taken=True,
      resource='XTA: 0x1b60c0 retires 0x337950[0..*0x337948): count 0 (only appender 0x1b6710 not reachable)')
    E('call', 'O', '0x1010dc', callee='0x1b5fd0', taken=True,
      resource='0x1b4c18 x *0x337610 (host), host frees 0x337620[0..*0x337920) (count 0), then 0x1b4b00 -> 0x1b2240')
    if retire_list:
        for i, h in enumerate(retire_list):
            B.ev('image_list_retire', 'O', '0x1b227c', index=i, handle=h, via='0x1b2240 -> 0x1cca30 (insertion order)')
            B.retire(h, '0x1b227c')
    B.ev('image_list_reset', 'O', '0x1b2294', count_before=len(retire_list), note='0x337608 = 0 (only if it was non-zero)')
    B.image_list = []
    E('call', 'O', '0x1b608c', callee='0x1b4a70', taken=True, resource='host: animation entries/counters reset')
    E('call', 'O', '0x1010e4', callee='0x1b3fc0', taken=True, resource='none')
    E('call', 'O', '0x1010ec', callee='0x18fb00', taken=True,
      resource='filter slots 0,3..8 cleared (1,2 kept); 0x18fa70(0), 0x1943a0, 0x192e90(0), 0x18fa80 (gate=1)')
    E('call', 'O', '0x1010f4', callee='0x1b8028', taken=True, resource='camera defaults; 0x337e18 = 0')

def mode8_construct_prefix(B, selector):
    E = B.ev
    E('construct', 'O', '0x1247d8', mode=8, selector=selector, node_descriptor='0x227988', node_bytes=0xd0,
      ctor='0x12c350', via='0x124818 phase 1 -> 0x1246b0 (mode table 0x223760[7] = 0x1247b0) -> 0x1151b8')
    E('store', 'O', '0x12c388', field='mode8+0x24', value=selector, meaning='selector = parent+0x44 (= queued +0x48)')
    E('construct', 'O', '0x12c384', node='resource 0x221980', node_bytes=0x50, ctor='0x100ef8', parent='mode8',
      note='appended to mode8 child list; 0x100ef8: +0x3c=1, +0x40=0x40c, +0x28=0x40d, +0x20=+0x48=-1, '
           '+0x24/+0x2c/+0x30/+0x34/+0x38/+0x44/+0x4c=0; 0x1cb4b8(0,0,0,0); 0x1cb538(0); 0x114cb0(0); 0x13c2d8; 0x197fc0')
    E('call', 'O', '0x12c390', callee='0x114c80', args='0.0', resource='fade target 0 -> 0x114cc0 enables filter slot 2')
    E('call', 'O', '0x12c3a0', callee='0x100c58', args='resource, 0x40f', resource='none (resource+0x40)')
    E('call', 'O', '0x12c3b4', callee='0x1cb4b8', args='0,0,0,0', resource='clear colour bytes of both layers')
    E('call', 'O', '0x12c3bc', callee='0x12d470', resource='none (closure)')
    E('mount', 'O', '0x12c3d0', directory='cmndata/2d/', archive='scr.xpf', via='0x1c0930, 0x1bccf0 (whole archive read)')

def mode8_construct_tail(B, selector):
    E = B.ev
    E('unmount', 'O', '0x12c50c', archive='scr.xpf', via='0x1bcc80; 0x1c0930(0) at 0x12c514')
    if selector == 1:
        return
    E('store', 'O', '0x12c784', field='mode8+0x2c', value=0)
    E('store', 'O', '0x12c7ec', field='mode8+0x30,+0xb8; 0x25990c=9999; 0x259908/10/14/18/1c=0; 0x1fb290=0', value=0)
    E('call', 'O', '0x12c894', callee='0x114cc0', args='1.0', resource='fade')
    E('return', 'O', '0x12c8b4', callee='0x114c80', args='0.0', note='tail call; constructor returns to 0x11528c')

def image_load(B, site, name, b, slot, list_pos):
    E = B.ev
    E('image_load', 'O', hex(site), name=name, via='0x100a60 -> 0x1009e0 -> 0x1b2348', archive='scr.xpf',
      file_read='0x1b2358 0x1c2e68("%s")' % name)
    h, r, x = B.load_xim(b, name, 'scr')
    E('host_free', 'O', '0x1b23b0', what='file buffer via *0x213580')
    B.image_list.append(h)
    B.counts['image_list_appends'] += 1
    E('image_list_append', 'O', '0x1b23dc', index=len(B.image_list) - 1, handle=h, list='0x337508', count_addr='0x337608')
    E('image_record', 'O', '0x100a1c', slot=slot, list_index=list_pos, handle=h, enabled=1,
      via='0x1b26a0 -> 0x1b25a8(slot, list_index|0x1000, 1): handle = list[list_index]; 0x1cc2b0 read for size')
    E('store', 'O', '0x100a38', field='resource+0x24/+0x2c/+0x30', value='1/%d/%d' % (slot + 1, list_pos + 1))
    return h

def build_events(elf, assets, initial):
    B = Builder(initial)
    E = B.ev
    scr = assets['scr_bytes']
    # --- traversal 1: parent queued {8,3,0} in 0x124940, no child --------------------------------------
    E('window_start', 'O', '0x100618', note='first scheduler traversal; Startup/RNG boundary state applies')
    E('traversal', 'O', '0x100618', label='T1', via='0x114f50 -> 0x114f70 (*0x1f85c0 root 0x223790)')
    E('parent', 'O', '0x124858', phase_before=0, action='request {8,3,0} pending, no child -> phase 1',
      store='0x1248c0 +0x20 = 1')
    E('main_loop_tail', 'O', '0x100628', writers=['W1', 'W2'], note='every iteration, see gs_writers')
    # --- traversal 2: construct selector 3 ------------------------------------------------------------
    E('traversal', 'O', '0x100618', label='T2')
    E('parent', 'O', '0x124878', phase_before=1, action='0x1007e0(0); current={8,3,0}; 0x24f738=8; construct; +0x3c=0; '
      '0x1008d8 (0x1cf910 occupancy probe, 0x1cc2a0 live handles: read-only); phase 0')
    mode8_construct_prefix(B, 3)
    E('store', 'O', '0x12c4c8', field='0x2597c9=6, 0x2597c8=0, 0x2597ca=0', value='storage request block')
    E('call', 'O', '0x12c4d4', callee='0x16c3c8', args='0x2597a8', resource='none: +0=0, +8=0x2000, +4=0')
    mode8_construct_tail(B, 3)
    E('tick', 'O', '0x12c2bc', callback='0x12bfc8 -> 0x12b508', selector=3, note='same traversal: mode8 callback '
      'after construction (children read after callbacks, 0x115018), then resource tick 0x100f90')
    E('tick_range', 'O', '0x12b508', selector=3, until='asynchronous storage completion (0x16c2a0 poll)',
      count='not static (U03)', resource='none (closure of 0x12b508 reaches no root)',
      states='0 -> 0x5e4 (0x16c370/0x16d580) -> ... -> 0x687 poll -> sufficient capacity -> 0x69a')
    E('request', 'O', '0x12bae8', selection=[8, 0, 0], via='0x124930(parent, 8, 0, 0)')
    # --- retirement of selector 3 ----------------------------------------------------------------------
    E('traversal', 'O', '0x100618', label='R3')
    E('parent', 'O', '0x124844', phase_before=0, action='request with child: child flags |= 0x80000000, +0x38 = 0')
    resource_dtor_events(B, 3, [])
    E('destructor', 'O', '0x11509c', node='mode8', dtor='0x12c8d8', selector=3, note='selector != 1: no stream stop')
    E('traversal', 'O', '0x100618', label='R3b')
    E('parent', 'O', '0x124858', phase_before=0, action='request, no child -> phase 1')
    E('node_free', 'O', '0x1150cc', nodes=['resource', 'mode8'], via='second dead pass: unlink + 0x1bdaf0 (host)')
    # --- construct selector 0 (logos) -------------------------------------------------------------------
    E('traversal', 'O', '0x100618', label='C0')
    E('parent', 'O', '0x124878', phase_before=1, action='construct {8,0,0}')
    mode8_construct_prefix(B, 0)
    logo = []
    for i, (site, sa) in enumerate(LOGO_SITES):
        name = STRING_ANCHORS[sa]
        logo.append(image_load(B, site, name, scr[name], i, i))
    mode8_construct_tail(B, 0)
    E('tick', 'O', '0x12c22c', callback='0x12bfc8 -> 0x129570', selector=0, counter=1, note='same traversal as construction')
    E('tick_range', 'O', '0x129570', selector=0, counters='1..599', resource='none',
      events={'1': 'image 0, rate +0.025', '180': 'rate -0.025', '270': 'shared init (below), image 1, rate +0.025',
              '480': 'rate -0.025', '<600': '0x1b3710(image, ...) submit'})
    E('shared_init', 'O', '0x1295d4', counter=270, calls=['0x12cde8 (JGL_BSC0.XV + six sequences)',
      '0x1bc538 flddata/event/evdata.xpf', '0x1bc538 flddata/obj/{hero,stun,rkumo,wkumo,bkumo}.xpf'],
      resource='host memory and sound only (no texture root in either closure)')
    E('request', 'O', '0x1296a0', counter=600, selection=[8, 1, 0], via='0x124930(parent, 8, 1, 0); no image submit at 600')
    E('traversal', 'O', '0x100618', label='R0')
    E('parent', 'O', '0x124844', phase_before=0, action='mark mode8 dead')
    resource_dtor_events(B, 0, list(B.image_list))
    E('destructor', 'O', '0x11509c', node='mode8', dtor='0x12c8d8', selector=0, note='selector != 1: nothing')
    E('traversal', 'O', '0x100618', label='R0b')
    E('parent', 'O', '0x124858', phase_before=0, action='phase 1')
    E('node_free', 'O', '0x1150cc', nodes=['resource', 'mode8'])
    # --- construct selector 1 (title) -------------------------------------------------------------------
    E('traversal', 'O', '0x100618', label='C1')
    E('parent', 'O', '0x124878', phase_before=1, action='construct {8,1,0}')
    mode8_construct_prefix(B, 1)
    title = []
    for i, (site, sa) in enumerate(TITLE_SITES):
        name = STRING_ANCHORS[sa]
        title.append(image_load(B, site, name, scr[name], i, i))
    E('store', 'O', '0x12c4b4', field='mode8+0xcc', value=title[-1],
      meaning='0x1b36c0(slot 7) = image record +4 = handle of hanten.xim')
    mode8_construct_tail(B, 1)
    E('mount', 'O', '0x12c794', directory='cmndata/2d/', archive='night_v.xpf', via='0x1c0930, 0x1bccf0')
    E('xmd_read', 'O', '0x186c54', name='a.xmd', via='0x12c7a0 0x186c48 -> 0x1c2e90 (whole decoded member, host)')
    names = assets['night_v_names']
    E('cache_state', 'O', '0x186b98', cache_enabled=0, lower_case_file_names=0, reserved_class=0,
      evidence='0x1fe310/0x1fe30c are 0 in the ELF; their only setters 0x186a58 (from 0x1bba88) and 0x186a48 (from '
               '0x131d28/0x13c860/0x152248) are outside the window and prefix closures; cls=1 only inside 0x178b50 '
               '(0x178be8..0x178c2c) while the main thread waits; 0x186aa8 runs on the main thread')
    first_res = B.model.reserve(len(names), 0)
    B.counts['reservations'] += 1
    E('reserve', 'H', '0x186b00', reserved_class=0, count=len(names), first=first_res,
      ids=list(range(first_res, first_res + len(names))), via='0x1cd5c8: queue B released (empty), first-fit HR')
    first = -1
    nv = assets['night_v_bytes']
    for i, nm in enumerate(names):
        fname = nm + '.xim'
        E('xmd_name', 'O', '0x186b2c', index=i, name=nm, file_name=fname, lookup=False,
          file_read='0x186bb8 0x1c2e68("%s")' % fname, member=fname.lower())
        h, r, x = B.load_xim(nv[fname.lower()], fname, 'night_v')
        slot, displaced = B.names.register(r, fname)
        B.counts['names_registered'] += 1
        E('name_register', 'O', '0x186be0', handle=h, record=r, stored=stored_name(fname), hash_slot=slot,
          displaced_record=displaced, via='0x1cc4e0 (newest duplicate wins)')
        E('host_free', 'O', '0x186bfc', what='member buffer via *0x213580')
        if first < 0:
            first = h
    E('xmd_names_return', 'O', '0x186c14', first_handle=first)
    E('lock', 'O', '0x18346c', via='0x179478(0x249890): PollSema 0x1fe294; contended -> 0x179230 + 0x178b50 window')
    E('mesh_slots', 'O', '0x182f60', allocator='*0x330140 (0x1cfc00 range allocator, host)', count=assets['night_v_meshes'],
      note='model slot range; value is allocator state (U01-like), returned as the 186c48 result')
    binding = xmd_binding(B.model, first, len(names))
    B.counts['bindings'] += 1
    E('xmd_binding', 'O', '0x182ffc', first_handle=first, count=len(names), handles=binding,
      record='host heap; linked at head 0x330144; +4 mesh slot base, +8 mesh count, +0xc count, +0x10 handles[]',
      via='0x182e28: handles[i]=h; h=0x1cc778(h) (successor in the live in-use list, not first+i)')
    E('host_free', 'O', '0x186c90', what='a.xmd buffer via *0x213580')
    E('call', 'O', '0x12c7bc', callee='0x1b8a80', args='0,0,0, mesh slot, flags 0', resource='none (instance table)',
      note='0x25995c = instance (0x12c7d0)')
    E('call', 'O', '0x12c7cc', callee='0x1bb728', resource='none')
    E('unmount', 'O', '0x12c7d4', archive='night_v.xpf', via='0x1bcc80; 0x1c0930(0)')
    E('store', 'O', '0x12c7e4', field='mode8+0x2c,+0x30,+0xb8; 0x25990c=9999; 0x259908/10/14/18/1c=0; 0x1fb290=0', value=0)
    E('call', 'O', '0x12c86c', callee='0x12db50', args='1, "snddata/str/thema_1"', resource='sound stream')
    E('call', 'O', '0x12c884', callee='0x12dea0', args='1.0, 0, 1, 0x80', resource='sound')
    E('call', 'O', '0x12c894', callee='0x114cc0', args='1.0', resource='fade')
    E('return', 'O', '0x12c8b4', callee='0x114c80', args='0.0', note='tail call; 0x12c350 returns to 0x11528c')
    E('stop', 'O', '0x11528c', note='STOP: after 0x12c350(selector=1) returns; next in the same traversal is 0x12bfc8 -> '
      '0x12c25c j 0x1296f8 (first title update, EXCLUDED)')
    return B, {'logo_handles': logo, 'title_handles': title, 'binding': binding, 'first': first}

def load_assets(local):
    base = os.path.join(local, 'xpf-decoded/CMNDATA/2D')
    scr = {}
    for sa in [s for _p, s in LOGO_SITES + TITLE_SITES]:
        n = STRING_ANCHORS[sa]
        scr[n] = open(os.path.join(base, 'SCR.XPF', n), 'rb').read()
    xmd = open(os.path.join(base, 'NIGHT_V.XPF', 'a.xmd'), 'rb').read()
    names = xmd_names(xmd)
    nv = {}
    for n in names:
        f = (n + '.xim').lower()
        nv[f] = open(os.path.join(base, 'NIGHT_V.XPF', f), 'rb').read()
    meshes = struct.unpack_from('<I', xmd, struct.unpack_from('<I', xmd, 0)[0])[0]
    packed = {k: sha(open(os.path.join(local, 'xpf-packed/CMNDATA/2D', k), 'rb').read())
              for k in ('SCR.XPF', 'NIGHT_V.XPF')}
    members = len([f for f in os.listdir(os.path.join(base, 'NIGHT_V.XPF')) if not f.startswith('.')])
    return {'night_v_members': members, 'scr_bytes': scr, 'xmd': xmd, 'night_v_names': names, 'night_v_bytes': nv, 'night_v_meshes': meshes,
            'packed': packed}

def asset_doc(a):
    def meta(name, b):
        x = parse_xim(b)
        return {'name': name, 'size': len(b), 'sha256': sha(b), 'psm': x['psm'], 'cpsm': x['cpsm'], 'levels': x['levels'],
                'colors': x['colors'], 'width': x['width'], 'height': x['height'], 'pixel_offset': x['pixel_offset'],
                'qwc': x['qwc'], 'payload_sha256': sha(x['pixels']),
                'tail_bytes': len(b) - x['pixel_offset'] - 16 * x['qwc']}
    groups = {}
    for i, n in enumerate(a['night_v_names']):
        groups.setdefault(stored_name(n + '.xim'), []).append(i)
    table = []
    for i, n in enumerate(a['night_v_names']):
        f = (n + '.xim').lower()
        m = meta(n + '.xim', a['night_v_bytes'][f])
        g = groups[stored_name(n + '.xim')]
        m.update(index=i, authored=n, member='local/xpf-decoded/CMNDATA/2D/NIGHT_V.XPF/' + f,
                 duplicate_group=g if len(g) > 1 else None)
        table.append(m)
    return {
        'scr_archive': {'runtime': 'cmndata/2d/scr.xpf', 'packed': 'local/xpf-packed/CMNDATA/2D/SCR.XPF',
                        'packed_sha256': a['packed']['SCR.XPF'], 'decoded_dir': 'local/xpf-decoded/CMNDATA/2D/SCR.XPF/'},
        'logos': [meta(STRING_ANCHORS[s], a['scr_bytes'][STRING_ANCHORS[s]]) for _p, s in LOGO_SITES],
        'title_images': [meta(STRING_ANCHORS[s], a['scr_bytes'][STRING_ANCHORS[s]]) for _p, s in TITLE_SITES],
        'night_v_archive': {'runtime': 'cmndata/2d/night_v.xpf', 'packed': 'local/xpf-packed/CMNDATA/2D/NIGHT_V.XPF',
                            'packed_sha256': a['packed']['NIGHT_V.XPF'],
                            'decoded_dir': 'local/xpf-decoded/CMNDATA/2D/NIGHT_V.XPF/', 'members': a['night_v_members']},
        'night_v_xmd': {'member': 'a.xmd', 'size': len(a['xmd']), 'sha256': sha(a['xmd']),
                        'name_table_offset': struct.unpack_from('<I', a['xmd'], 4)[0], 'names': len(a['night_v_names']),
                        'distinct_case_insensitive': len(groups), 'meshes': a['night_v_meshes'],
                        'duplicate_groups': [g for g in groups.values() if len(g) > 1]},
        'night_v_textures': table,
    }

KINDS = {'window_start', 'traversal', 'parent', 'main_loop_tail', 'construct', 'store', 'call', 'mount', 'unmount',
         'return', 'tick', 'tick_range', 'request', 'destructor', 'image_list_retire', 'image_list_reset', 'node_free',
         'image_load', 'alloc', 'handle_create', 'ref_adjust', 'upload', 'palette_bind', 'host_free', 'image_list_append',
         'image_record', 'shared_init', 'handle_retire', 'free', 'record_release', 'handle_id_free', 'xmd_read',
         'cache_state', 'reserve', 'xmd_name', 'name_register', 'xmd_names_return', 'lock', 'mesh_slots', 'xmd_binding',
         'stop'}
PSMS = (0, 1, 19, 20, 36, 44, 49)

def check(doc):
    """Return a list of problems (empty = valid and internally consistent)."""
    bad = []

    def need(c, m):
        if not c:
            bad.append(m)
    need(doc.get('schema') == SCHEMA, 'schema')
    need(doc.get('elf', {}).get('sha256') == ELF_SHA256, 'elf sha256')
    ev = doc.get('events')
    init = doc.get('initial_state')
    need(isinstance(ev, list) and ev, 'events missing')
    need(isinstance(init, dict), 'initial_state missing')
    if not isinstance(ev, list) or not isinstance(init, dict):
        return bad
    need(ev[0].get('kind') == 'window_start' and ev[0].get('pc') == '0x100618', 'window must start at 0x100618')
    need(ev[-1].get('kind') == 'stop', 'last event must be the stop')
    need(sum(1 for e in ev if e.get('kind') == 'stop') == 1, 'exactly one stop')
    refs = dict(init['live_allocations'])
    model = HandleModel(init['live_handles_in_order'], init['handle_records'])
    names = NameHash()
    allocs, uploaded, freed = {}, set(), set()
    handle_img, image_list = {}, []
    reserved_ids = []
    for i, e in enumerate(ev):
        need(e.get('seq') == i, 'seq %d' % i)
        k = e.get('kind')
        need(k in KINDS, 'kind %r' % (k,))
        need(e.get('tag') in TAGS, 'tag at seq %d' % i)
        need(isinstance(e.get('pc'), str) and e['pc'].startswith('0x'), 'pc at seq %d' % i)
        if k == 'alloc':
            need(e['id'] not in refs and e['id'] not in allocs, 'duplicate alloc id ' + e['id'])
            need(e['alloc_class'] in (0, 1, 2, 3), 'class')
            need(e['psm'] in PSMS, 'psm %r' % e['psm'])
            need(e['width'] > 0 and e['height'] > 0, 'dims')
            need(e['initial_references'] == 0, 'allocations start with 0 references')
            allocs[e['id']] = e
            refs[e['id']] = 0
        elif k == 'ref_adjust':
            a = e.get('alloc')
            need(a in refs and a not in freed, 'ref of unknown/freed alloc %s' % a)
            if a in refs:
                refs[a] += e['delta']
                need(refs[a] == e['references_after'], 'references_after mismatch seq %d' % i)
                need(refs[a] >= 0, 'negative refs seq %d' % i)
        elif k == 'free':
            a = e.get('alloc')
            need(refs.get(a, 1) < 1, 'free of a referenced allocation seq %d' % i)
            need(a not in freed, 'double free')
            freed.add(a)
        elif k == 'upload':
            a = allocs.get(e['alloc'])
            need(a is not None, 'upload to unknown alloc seq %d' % i)
            if a:
                need((e['rrw'], e['rrh'], e['psm']) == (a['width'], a['height'], a['psm']), 'upload rect seq %d' % i)
                need(e['qwc'] == qwc(e['psm'], e['rrw'], e['rrh']), 'qwc seq %d' % i)
                need(e['bytes'] == 16 * e['qwc'], 'bytes seq %d' % i)
                need(e['alloc'] not in uploaded, 'second upload')
                need(len(e.get('sha256', '')) == 64, 'upload hash')
                uploaded.add(e['alloc'])
        elif k == 'reserve':
            s = model.reserve(e['count'], e['reserved_class'])
            need(e['reserved_class'] == 0, 'only reserved class 0 is reachable')
            need(s == e['first'] and e['ids'] == list(range(s, s + e['count'])), 'reservation ids seq %d' % i)
            reserved_ids = list(e['ids'])
        elif k == 'handle_create':
            need(e['image_alloc'] in allocs, 'handle image alloc seq %d' % i)
            if e.get('from_reserved_queue'):
                need(reserved_ids and model.queue[0] and model.queue[0][0] == e['handle'], 'reserved queue head seq %d' % i)
            h, r = model.create(e['reserved_class'])
            need((h, r) == (e['handle'], e['record']), 'handle/record ids seq %d: model %s' % (i, (h, r)))
            handle_img[h] = e['image_alloc']
        elif k == 'palette_bind':
            need(e['handle'] in model.map and e['alloc'] in allocs, 'bind refs seq %d' % i)
        elif k == 'image_list_append':
            need(e['index'] == len(image_list), 'image list index seq %d' % i)
            image_list.append(e['handle'])
        elif k == 'image_record':
            need(0 <= e['list_index'] < len(image_list) and image_list[e['list_index']] == e['handle'],
                 'image record handle seq %d' % i)
        elif k == 'image_list_retire':
            need(e['index'] < len(image_list) and image_list[e['index']] == e['handle'], 'retire order seq %d' % i)
        elif k == 'image_list_reset':
            need(e['count_before'] == len(image_list), 'image list count seq %d' % i)
            image_list = []
        elif k == 'handle_retire':
            need(e['handle'] in model.map, 'retire of dead handle seq %d' % i)
            if e['handle'] in model.map:
                r, last = model.retire(e['handle'])
                need((r, last) == (e['record'], e['last_handle']), 'retire record seq %d' % i)
        elif k == 'name_register':
            slot, displaced = names.register(e['record'], e['stored'])
            need(e['stored'] == stored_name(e['stored']), 'stored name form seq %d' % i)
            need((slot, displaced) == (e['hash_slot'], e['displaced_record']), 'name hash seq %d' % i)
        elif k == 'xmd_binding':
            need(e['handles'] == xmd_binding(model, e['first_handle'], e['count']), 'successor binding seq %d' % i)
        elif k == 'cache_state':
            need(e['cache_enabled'] == 0 and e['reserved_class'] == 0, 'cache/class state')
    # final state
    fr = doc.get('final_state', {})
    need(fr.get('references') == {a: r for a, r in refs.items() if a not in freed}, 'final references')
    need(fr.get('live_handles_in_order') == model.in_use, 'final in-use handle order')
    need(fr.get('freed_allocations') == sorted(freed), 'freed allocations')
    need(fr.get('image_list') == image_list, 'final image list')
    need(fr.get('reserved_queue_0') == model.queue[0] and fr.get('reserved_queue_1') == model.queue[1], 'final queues')
    need(fr.get('hash_entries') == sorted([[s, r + 8] for r, s in names.slot.items() if s >= 0]), 'final hash entries')
    c = doc.get('counts', {})
    cnt = {
        'allocations': len(allocs), 'uploads': len(uploaded), 'frees': len(freed),
        'handles_created': sum(1 for e in ev if e['kind'] == 'handle_create'),
        'handles_retired': sum(1 for e in ev if e['kind'] == 'handle_retire'),
        'records_destroyed': sum(1 for e in ev if e['kind'] == 'record_release'),
        'palette_binds': sum(1 for e in ev if e['kind'] == 'palette_bind'),
        'reservations': sum(1 for e in ev if e['kind'] == 'reserve'),
        'names_registered': sum(1 for e in ev if e['kind'] == 'name_register'),
        'bindings': sum(1 for e in ev if e['kind'] == 'xmd_binding'),
        'image_list_appends': sum(1 for e in ev if e['kind'] == 'image_list_append'),
        'events': len(ev),
    }
    for key, v in cnt.items():
        need(c.get(key) == v, 'count %s: %r != %r' % (key, c.get(key), v))
    # assets / NIGHT_V table
    a = doc.get('assets', {})
    tex = a.get('night_v_textures', [])
    xn = [e for e in ev if e['kind'] == 'xmd_name']
    need(len(tex) == len(xn) == a.get('night_v_xmd', {}).get('names'), 'NIGHT_V name table length')
    for t, e in zip(tex, xn):
        need(t['authored'] == e['name'] and t['index'] == e['index'], 'name table order %s' % t.get('authored'))
        need(t['psm'] == 1 and t['levels'] == 0 and t['colors'] == 0, 'NIGHT_V must be PSM1 without CLUT')
        need(t['qwc'] == qwc(1, t['width'], t['height']), 'NIGHT_V qwc')
    groups = {}
    for t in tex:
        groups.setdefault(stored_name(t['authored'] + '.xim'), []).append(t['index'])
    need(sorted(g for g in groups.values() if len(g) > 1) == sorted(a.get('night_v_xmd', {}).get('duplicate_groups', [])),
         'duplicate groups')
    for t in tex:
        g = groups[stored_name(t['authored'] + '.xim')]
        need(t['duplicate_group'] == (g if len(g) > 1 else None), 'duplicate group of %s' % t['authored'])
    need([i['name'] for i in a.get('title_images', [])] ==
         ['title_sample4.xim', 'push.xim', 'new.xim', 'restore.xim', 'maru_w.xim', 'maru_b.xim', 'sp.xim', 'hanten.xim'],
         'title image order')
    need([i['name'] for i in a.get('logos', [])] == ['scea.xim', 'zener.xim'], 'logo order')
    for g in doc.get('gs_writers', []):
        need(g.get('tag') in TAGS and g.get('id') and isinstance(g.get('regions'), list), 'gs writer entry')
    need(doc.get('unresolved'), 'unresolved list empty')
    need(doc.get('partial_failures'), 'partial failures missing')
    need(doc.get('native_mapping'), 'native mapping missing')
    return bad

# ---------------------------------------------------------------------------

NATIVE_MAPPING = [
    {'step': 'scheduler parent / dead-node traversal', 'source': '0x124818/0x124940/0x114f70',
     'native': 'include/okage/game_mode.hpp:12, src/game_mode.cpp:4-31; driven at src/native_runtime.mm:1212',
     'status': 'implemented (phases, dead allocation one extra traversal)', 'missing': 'no source-event observation hooks'},
    {'step': 'mode8 construction dispatch', 'source': '0x1246b0 -> 0x1151b8 -> 0x12c350',
     'native': 'src/native_runtime.mm:1314-1321 (selectors 3/0 -> StartupSession), :1322-1336 (selector 1 -> TitleSession)',
     'status': 'implemented as host objects', 'missing': 'resource-child construction 0x100ef8 and its stores are not modeled as events'},
    {'step': 'selector 3 storage', 'source': '0x12b508 states 0/0x5e4/0x687/0x69a; request 0x12bae8',
     'native': 'src/startup_session.mm:69-82 (NativeLoadQueue probe, request {8,0,0})',
     'status': 'implemented (asynchronous)', 'missing': 'scr.xpf mount/unmount in the selector-3 constructor not represented'},
    {'step': 'selector 0 logo loads', 'source': '0x12c408/0x12c418 -> 0x1009e0 -> 0x1b2348 -> 0x18f7a8',
     'native': 'src/startup_session.mm:55-60 (construction.load_image -> Metal upload); loader at src/native_runtime.mm:1317-1319',
     'status': 'host upload only', 'missing': 'no StartupTextureOwner allocation/handle/upload/CLUT/bind events, no 0x337508 list'},
    {'step': 'logo ticks 1/180/270/480/600', 'source': '0x129570',
     'native': 'src/startup.cpp:10-33; tick 270 service src/native_runtime.mm:703-716',
     'status': 'implemented', 'missing': '-'},
    {'step': 'resource retirement 0x101058 (selector 3 and 0)', 'source': '0x101058 -> 0x1b5fd0 -> 0x1b4b00 -> 0x1b2240',
     'native': 'src/native_runtime.mm:1214-1216 -> src/startup_session.mm:118-120 (resource.retire)',
     'status': 'lifecycle only', 'missing': 'no 1cca30 of the two logo handles, no descriptor releases/frees, no list reset'},
    {'step': 'wrapper free (second dead pass)', 'source': '0x1150cc/0x1150e8',
     'native': 'src/native_runtime.mm:1267-1270', 'status': 'implemented', 'missing': '-'},
    {'step': 'title SCR images', 'source': '0x12c430..0x12c4a0 -> 0x1009e0 -> 0x1b2348',
     'native': 'src/title_session.mm:53-56 (re-reads common_root/2D/SCR.XPF, Metal upload)',
     'status': 'host upload only', 'missing': 'source events, handles 36..43, list 0x337508, image records'},
    {'step': 'mode8+0xcc hanten handle', 'source': '0x12c4a8 0x1b36c0',
     'native': 'src/title_session.mm:58 (task.texture = effect texture index of images[7])',
     'status': 'native texture index, not the source handle', 'missing': 'source handle value'},
    {'step': 'shared effect textures for title', 'source': '(no source event: startup handles reused)',
     'native': 'src/title_session.mm:57 (load_effect_textures re-reads ENV_TEX.XPF from disk)',
     'status': 'divergent provenance', 'missing': 'reuse verified startup bytes; no second logical allocation'},
    {'step': 'NIGHT_V XMD names / reservation / loads / names', 'source': '0x186c48 -> 0x186aa8',
     'native': 'TextureHandleOwner::load_xmd_names src/texture_handles.cpp:364-398 (not wired); title reads the model at '
               'src/title_session.mm:59-61',
     'status': 'API exists, unused by TitleSession', 'missing': 'PSM1 (PSMCT24) create_image (src/texture_handles.cpp:135-137 '
               'rejects it); case-insensitive member lookup for the reader'},
    {'step': 'XMD material binding 0x183450 -> 0x182f08 -> 0x182e28 / 0x1cc778', 'source': '0x182e28',
     'native': 'src/scene_renderer.mm:287-295 (inputTextures: decode each name from disk, by name)',
     'status': 'by-name file decode', 'missing': 'in-use-list successor (1cc778), binding record, handle->descriptor TEX0 at build'},
    {'step': 'model instance 0x1b8a80/0x1bb728', 'source': '0x12c7bc', 'native': 'src/title_session.mm:60-61',
     'status': 'host actor', 'missing': '-'},
    {'step': 'title theme 0x12db50/0x12dea0', 'source': '0x12c86c', 'native': 'src/native_runtime.mm:1325-1333 (start_theme)',
     'status': 'implemented', 'missing': '-'},
    {'step': 'fade 0x114c80/0x114cc0 (filter slot 2)', 'source': '0x12c390, 0x12c894, 0x12c8b4',
     'native': 'SceneFade in src/title_session.mm:47,69 and src/startup_session.mm:49,67', 'status': 'implemented', 'missing': '-'},
    {'step': 'GS memory coverage', 'source': 'gs_writers W1..W6',
     'native': 'include/okage/startup_textures.hpp:25-35 (StartupCoverage::PreSchedulerOnly)',
     'status': 'pre-scheduler only', 'missing': 'invalidate A00/A01/A02 low lanes from the first iteration; A03 conditional'},
]

CONFIDENCE = [
    ('scheduler/parent/mode8 node machinery and traversal order', 'high', 'listings 0x114f70, 0x124818, 0x1246b0, 0x1151b8'),
    ('constructor bodies for selectors 3/0/1 (calls, order, arguments)', 'high', 'listing 0x12c350 + ELF strings/tables'),
    ('per-tick callbacks (0x129570 ticks; 0x12b508 completion request)', 'high', 'listings; storage tick count async (U03)'),
    ('resource retirement order and logo handle release in insertion order', 'high', '0x101058, 0x1b5fd0, 0x1b2240, 0x1cca30'),
    ('no other resource effect in either retirement', 'medium-high', 'closure + writer xrefs (U02, U07)'),
    ('image/CLUT/handle/upload/bind events of 18f7a8 loads', 'high', 'Handle/Startup routines; formats from owned bytes'),
    ('handle ids / record indices', 'medium', 'derived (U06)'),
    ('cache off, lower-case off, class 0 during 0x186aa8, no XTA', 'high (static)', 'setter closures; thread/window reading (U02)'),
    ('NIGHT_V name table, duplicates, PSM1, sizes', 'high', 'parsed from owned a.xmd and members (U08 for lookup)'),
    ('1cc778 successor binding algorithm', 'high', 'listings 0x182e28, 0x1cc778; slot values derived (U06)'),
    ('material slot -> name index -> handle -> descriptor at build', 'medium-high', '0x17be50/0x17bda8 read side (U09)'),
    ('GS writer classification', 'medium', 'packet builders read; register bits not decoded (U04, U05)'),
    ('partial-failure semantics', 'high (reading)', 'not exercised'),
]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--local')
    ap.add_argument('--elf')
    ap.add_argument('--startup', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs',
                                                  'startup-contract.json'))
    ap.add_argument('--out')
    ap.add_argument('--check')
    a = ap.parse_args()
    if a.check:
        probs = check(json.load(open(a.check)))
        for p in probs:
            print('FAIL', p)
        print('OK' if not probs else '%d problems' % len(probs))
        return 1 if probs else 0
    if not a.local:
        ap.error('--local or --check is required')
    elf = Elf(a.elf or os.path.join(a.local, 'extracted/SCUS_971.29'))
    bad = verify_elf(elf)
    if bad:
        sys.exit('ELF anchors failed:\n' + '\n'.join(bad))
    startup_bytes = open(a.startup, 'rb').read()
    startup = json.loads(startup_bytes)
    initial = build_initial_state(startup)
    assets = load_assets(a.local)
    B, summary = build_events(elf, assets, initial)
    freed = sorted(e['alloc'] for e in B.events if e['kind'] == 'free')
    doc = {
        'schema': SCHEMA,
        'elf': {'path': 'local/extracted/SCUS_971.29', 'sha256': elf.sha256},
        'boundary': {'start': '0x100618 (first scheduler traversal call)', 'start_state': 'Startup/RNG verified prefix',
                     'stop': 'return of 0x12c350(selector=1) to 0x11528c (inside 0x1151b8 <- 0x1246b0 <- 0x124818 phase 1)',
                     'excluded_next': '0x12c8f8 j 0x12bfc8 -> 0x12c25c j 0x1296f8 (first title update)'},
        'baseline': {'startup_contract': 'docs/startup-contract.json', 'startup_sha256': sha(startup_bytes)},
        'initial_state': initial,
        'scheduler': {
            'root': {'descriptor': '0x223790', 'ctor': '0x124940 (queues {8,3,0} via 0x124930)', 'cb1': '0x124988 -> 0x124818',
                     'dtor': '0x124980', 'store': '*0x1f85c0 (0x115150)'},
            'node_layout': '+0 cb0, +4 cb1, +8 dtor, +0xc prev, +0x10 next, +0x14 parent, +0x18 first child, +0x1c flags',
            'flags': '0x80000000 dead; 0x40000000 toggled on the first dead pass (children marked dead, recurse, dtor) and '
                     'cleared on the second (recurse, unlink, 0x1bdaf0); 0x04000000 skip callbacks; 0x10000000 skip cb0; '
                     '0x08000000 skip cb1; children are read AFTER the callbacks (0x115018), so a child constructed in a '
                     'callback runs in the same traversal',
            'parent_phases': '0: request && child -> mark child dead (+0x38=0); request && !child -> phase 1. '
                             '1: 0x1007e0(0); current=+0x3c; 0x24f738=mode; +0x44=+0x48 (selector); +0x4c=+0x50; '
                             'child=0x1246b0(parent, mode); +0x3c=0; 0x1008d8; phase 0',
            'mode8': {'descriptor': '0x227988', 'bytes': 0xd0, 'ctor': '0x12c350', 'cb1': '0x12c8f8 -> 0x12bfc8',
                      'dtor': '0x12c8d8 (selector 1 only: 0x12dd50(1))',
                      'update_switch': {'0': '0x129570 logos', '1': '0x1296f8 title (excluded)', '3': '0x12b508 storage'}},
            'resource_child': {'descriptor': '0x221980', 'bytes': 0x50, 'ctor': '0x100ef8', 'cb1': '0x100f90',
                               'dtor': '0x101058'},
            'requests': [{'pc': '0x124974', 'selection': [8, 3, 0]}, {'pc': '0x12bae8', 'selection': [8, 0, 0]},
                         {'pc': '0x1296a0', 'selection': [8, 1, 0]}],
            'traversal_sequence': ['T1 phase0->1', 'T2 construct 3 (+tick)', 'storage ticks (async)', 'request {8,0,0}',
                                   'R3 mark dead + dtors 0x101058, 0x12c8d8', 'R3b phase 1 + node frees',
                                   'C0 construct 0 (+tick 1)', 'logo ticks 2..600', 'R0 dtors (logo retire)',
                                   'R0b phase 1 + node frees', 'C1 construct 1 -> STOP at return'],
            'reserved_queue_class': {
                'variable': '0x3abb14 written only by 0x1cbdd8 (registered in 0x178910 table)',
                'window': '0x178b50 on the main thread: 0x178be8 178910(1) -> 0x178bfc ResumeThread(worker *0x31fa50) -> '
                          '0x178c08 WaitSema(*0x31fa40) -> 0x178c10 SuspendThread(worker) -> 0x178c2c 178910(0)',
                'run': '0x178408 runs jobs from ring 0x31faf0 (head 0x1fe244/tail 0x1fe248); only poster 0x178578 '
                          '(callers 0x12f008 0x130920 0x130a0c 0x133a14 0x13c9f8 0x14a110) is outside the prefix and window '
                          'closures',
                'other_threads': '0x1784c0 idle, 0x16d918 card, 0x1c5340, 0x1d4b78, 0x1e9920: no handle root in closure',
                'conclusion': 'class 0 (queue B) for every handle creation and the 0x186aa8 reservation; class 1 (queue C) '
                              'is not reachable in this prefix'},
            'cache': {'0x1fe310': '0 (ELF); setter 0x186a58 <- 0x1bbac8 in 0x1bba88 <- 0x1bc070/0x1bc130, none reachable',
                      '0x1fe30c': '0 (ELF); setter 0x186a48 <- 0x131d28/0x13c860/0x152248, none reachable',
                      '0x1fe308': '0x18f7a8 (ELF); setter 0x186a30 has no caller',
                      'conclusion': 'title route bypasses 0x1bba88: no 0x1cce78 lookup, every name loads, duplicates '
                                    'become separate handles/records/allocations'},
        },
        'main_loop_per_iteration': [{'pc': p, 'callee': c, 'note': n} for p, c, n in MAIN_LOOP],
        'events': B.events,
        'assets': asset_doc(assets),
        'binding': {'first_handle': summary['first'], 'handles': summary['binding'],
                    'algorithm': 'material record (0x3301a4, 0x18 bytes) +0 s16 texture index t -> 0x17bda8: t in '
                                 '[0, count-1] ? record(*0x330144).handles[t] : 0 -> 0x1cc2b0 image descriptor, '
                                 '0x1cd500(h, +2 level) palette -> TEX0 packed at packet build (0x17bf84); handles[] from '
                                 '0x182e28 = first, 1cc778(first), ... (live in-use list order)'},
        'gs_writers': GS_WRITERS,
        'not_gs_writers': NOT_GS_WRITERS,
        'partial_failures': PARTIAL_FAILURES,
        'native_mapping': NATIVE_MAPPING,
        'final_state': {
            'references': {k: v for k, v in B.refs.items() if k not in freed},
            'freed_allocations': freed,
            'live_handles_in_order': list(B.model.in_use),
            'image_list': list(B.image_list),
            'reserved_queue_0': list(B.model.queue[0]), 'reserved_queue_1': list(B.model.queue[1]),
            'hash_entries': sorted([[s, r + 8] for r, s in B.names.slot.items() if s >= 0]),
            'mode8_0xcc_handle': summary['title_handles'][-1],
        },
        'counts': dict(B.counts, events=len(B.events)),
        'confidence': [{'step': s, 'confidence': c, 'basis': b} for s, c, b in CONFIDENCE],
        'unresolved': [{'id': i, 'text': t} for i, t in UNRESOLVED],
    }
    doc['counts']['frees'] = len(freed)
    probs = check(doc)
    if probs:
        sys.exit('generated contract fails its own check:\n' + '\n'.join(probs))
    out = json.dumps(doc, indent=1)
    if a.out:
        open(a.out, 'w').write(out + '\n')
    else:
        print(out)
    return 0

if __name__ == '__main__':
    sys.exit(main())
