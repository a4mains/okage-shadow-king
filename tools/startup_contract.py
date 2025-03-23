#!/usr/bin/env python3
"""Generate and verify the Startup startup texture-resource event contract.

Pure Python 3 (standard library only).  The contract itself is the curated result of
static Capstone reading of the pinned ELF (see the startup input contract); this
tool (a) re-checks the curated anchors against the ELF, (b) recomputes the call
closure evidence, (c) recomputes every asset/payload hash from the owned data, and
(d) writes docs/startup-contract.json.  Without --local it only validates a JSON.

  python3 tools/startup_contract.py --local /path/to/okage/local \
      [--model-jsonl FILE] --out docs/startup-contract.json
  python3 tools/startup_contract.py --check docs/startup-contract.json

Nothing here executes game code.
"""
import argparse
import hashlib
import json
import struct
import sys

ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
SCHEMA = 'okage.startup-contract.v1'
MAIN = 0x1002f8
FIRST_SCHEDULER_CALL = 0x100618
SCHEDULER_ENTRY = 0x114f50

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
        b = bytearray()
        while True:
            c = self.rb(va + len(b), 1)[0]
            if not c:
                return bytes(b).decode('latin1')
            b.append(c)

RES_ROOTS = {
    0x1cf008: 'alloc', 0x1cee50: 'adjust_refs', 0x1cf728: 'free_if_unreferenced',
    0x1cebe8: 'clear_reservation', 0x1ce940: 'vram_allocator_init',
    0x1cba48: 'gs_image_upload', 0x1cd1e0: 'image_alloc_handle_upload',
    0x1cd7d0: 'clut_alloc_upload', 0x1cd298: 'palette_bind', 0x1cc880: 'handle_create',
    0x1cc4e0: 'handle_name', 0x1cd5c8: 'handle_reserve', 0x1cca30: 'handle_retire',
    0x1ccd48: 'handle_alias', 0x1cce78: 'name_lookup', 0x1cbdf8: 'handle_system_init',
    0x1cc2f8: 'record_image_replace', 0x1cdb80: 'color_layer_alloc',
    0x1cde18: 'attach_depth', 0x1cdda0: 'attach_color',
}
PRUNE = {0x1c7910, 0x178a70, 0x178b18}  # printf-like logger, lock, unlock

class Graph:
    def __init__(self, elf, functions_tsv):
        self.elf = elf
        self.sizes = {}
        for line in open(functions_tsv).read().split('\n')[1:]:
            if line:
                p = line.split('\t')
                self.sizes[int(p[0], 16)] = max(int(p[2]), int(p[3]))
        self.starts = sorted(self.sizes)
        self.cache = {}

    def extent(self, a):
        import bisect
        if a in self.sizes:
            return a, a + self.sizes[a]
        i = bisect.bisect_right(self.starts, a)
        return a, (self.starts[i] if i < len(self.starts) else a + 0x1000)

    def callees(self, a):
        if a in self.cache:
            return self.cache[a]
        s, e = self.extent(a)
        code = self.elf.rb(s, e - s)
        out, jalr = [], 0
        for o in range(0, len(code) - 3, 4):
            w = struct.unpack_from('<I', code, o)[0]
            pc = s + o
            op = w >> 26
            if op in (2, 3):
                t = ((w & 0x3ffffff) << 2) | ((pc + 4) & 0xf0000000)
                if op == 2 and s <= t < e:
                    continue
                out.append(t)
            elif op == 0 and (w & 63) == 9:
                jalr += 1
        self.cache[a] = (out, jalr)
        return self.cache[a]

    def closure(self, a):
        seen = set()
        stack = [a]
        while stack:
            x = stack.pop()
            if x in seen:
                continue
            seen.add(x)
            if x in PRUNE:
                continue
            stack.extend(self.callees(x)[0])
        return seen

    def summarize(self, a):
        cl = self.closure(a)
        return {
            'closure_functions': len(cl),
            'jalr_sites_in_closure': sum(self.callees(x)[1] for x in cl if x not in PRUNE),
            'resource_roots_reached': sorted(RES_ROOTS[x] for x in cl if x in RES_ROOTS),
        }

TOP = [
    (0x100320, 0x1ed1c0, 'none', 'role unresolved (2-function closure, 1 jalr)'),
    (0x100328, 0x1c7730, 'a0=argv[0] pointer ([a1])', 'library/system init; closure reaches DMA/IOP helpers only'),
    (0x100330, 0x1c6918, 'none', 'library/system init; closure reaches DMA/IOP helpers only'),
    (0x100338, 0x1cac18, 'none', 'leaf'),
    (0x100340, 0x1bf300, 'none', 'memory/arena helper family'),
    (0x100348, 0x1c0638, 'a0=0', 'leaf'),
    (0x100350, 0x1bf468, 'a0=1', 'leaf'),
    (0x100358, 0x1bf4a0, 'a0=1', 'leaf'),
    (0x100360, 0x115a98, 'none', 'leaf; followed by stores 0x1f85c8+0x18=0x20, +0x14=0x40'),
    (0x100380, 0x1bd2a0, 'a0=0', 'lock/unlock wrapper'),
    (0x100388, 0x1bf218, 'none', 'memory/arena helper family'),
    (0x100390, 0x1f0880, 'a0=0x180000', 'returns a pointer used by the next two calls'),
    (0x1003a0, 0x1bd470, 'a0=prev result, a1=0x180000, a2=0x400', 'arena/heap region initialiser (host RAM)'),
    (0x1003a8, 0x1bd900, 'a0=prev result', 'arena/heap helper'),
    (0x1003b0, 0x16c1e0, 'none', 'role unresolved; followed by word store 0x70000010 -> 0x70000000'),
    (0x1003c4, 0x1c8b40, 'none', 'DMA sync helper family'),
    (0x1003d0, 0x1c78c8, 'a0=0x1002f0', 'registers an error-string/callback pointer'),
    (0x1003d8, 0x1cb088, 'none', 'VRAM allocator init (1ce940), GS global init (1cb0e8), handle system init (1cc248->1cbdf8(0x200,0x80)), palette mode 0 (1cd798->1cd7a8(0))'),
    (0x1003e0, 0x1ce7e8, 'a0=0', 'stores 0x2138c8 (allocator debug mask) under lock'),
    (0x1003e8, 0x1cff30, 'a0=0', 'DMA/queue sync'),
    (0x1003f0, 0x1cbbf8, 'a0=0', 'GS PMODE write (display circuit enable), 3a177c'),
    (0x100404, 0x1cb1c0, 'a0=0,a1=1,a2=2,a3=1', 'GS CRT/display mode (tail j 1e0348), no VRAM'),
    (0x10040c, 0x1cbbf8, 'a0=0', 'GS PMODE write'),
    (0x100428, 0x1cb1e8, 'a0=0,a1=0x280,a2=0xe0,a3=3,t0=0x31,t1=1', 'STEP 1: two 640x224 colour layers + shared PSM49 depth'),
    (0x10043c, 0x1cb4b8, 'a0=a1=a2=0x40,a3=0', 'layer clear colour R=G=B=0x40,A=0 (bytes 0xc0..0xc3 of both layers)'),
    (0x100444, 0x1cb538, 'a0=0', 'stores 0 to layer words +0xe4/+0xd4 of both layers'),
    (0x10044c, 0x1f0880, 'a0=0x200000', 'returns pointer for 1c8f10'),
    (0x100458, 0x1c8f10, 'a0=prev result, a1=0x200000', 'DMA list pool init (host RAM)'),
    (0x100460, 0x179340, 'none', 'leaf'),
    (0x100468, 0x1ace90, 'none', 'VU1 microprogram upload packet (1b0168->1b00f0->1afa40 VIF MPG) kicked by 1cad20; no GS memory'),
    (0x100470, 0x1b1e60, 'none', 'VU1 microprogram upload (1b1e90->1ac620->1ac568) kicked via 1ac210->1cad20; no GS memory'),
    (0x100478, 0x1795e8, 'none', 'VU1 microprogram upload (1b14c0->1b01f8) via 1ac568; plus 179418/1835f8/184410/1d2ad0 (CPU tables)'),
    (0x100480, 0x1798e0, 'a0=0x80', 'small helper'),
    (0x100488, 0x194640, 'none', 'font-system init: 1946b0(0x10) allocates host table (no VRAM; retire path 1cca30 skipped because table pointer is null), 197128, tail 1955f0(1.0f,1.0f)'),
    (0x100490, 0x1974f8, 'a0=0x100', 'STEP 1b (NOT in handoff prefix): glyph-cache texture 1cd1e0(0x14,0x400,0x100,NULL) + CLUT alloc/upload/bind'),
    (0x100498, 0x1b3f50, 'none', 'no root reached'),
    (0x1004a0, 0x1b20c8, 'none', 'no root reached'),
    (0x1004a8, 0x1b5b20, 'none', 'no root reached'),
    (0x1004b0, 0x1145a0, 'none', 'resets two auxiliary texture ids to -1 (per the battle resource integration contract); no root reached'),
    (0x1004b8, 0x18fc68, 'a0=0', 'STEP 2: capture/filter init (flags = ~0)'),
    (0x1004c0, 0x186dc8, 'none', 'no root reached'),
    (0x1004d0, 0x18e4c0, 'f12=10.0f', 'binds materials of two embedded models via 183450(xmd,first=0) (reads handles; no allocator/ref-count root reached); stores 0x1fe340+.. and 0x332920=10.0f'),
    (0x1004d8, 0x1b6130, 'none', 'no root reached'),
    (0x1004e0, 0x1b8388, 'none', 'no root reached'),
    (0x1004f4, 0x1bb040, 'a0=3,a1=0x209,a2=0x205,a3=9', 'no root reached'),
    (0x1004fc, 0x1250b0, 'none', 'no root reached'),
    (0x100504, 0x125130, 'none', 'loads field database (per native comment); no root reached'),
    (0x10050c, 0x1251d8, 'none', 'no root reached'),
    (0x100514, 0x1011d0, 'none', 'STEP 3: shared effects (103a88(0)->103718)'),
    (0x10051c, 0x114c20, 'none', 'leaf'),
    (0x100524, 0x1001a8, 'none', 'STEP 4: three fonts (returns 1)'),
    (0x100534, 0x114e18, 'none', 'no root reached'),
    (0x100544, 0x115120, 'a0=0x223790,a1=0xa0', 'creates scheduler root, stores 0x1f85c0 (non-null => 114f50 traverses)'),
    (0x100554, 0x1bf5a0, 'none', 'no root reached'),
    (0x10055c, 0x12ccd0, 'none', 'startup sound slot 0 load (per native comment); no root reached'),
    (0x100568, 0x12e1c8, 'a0=argc-1,a1=argv+4', 'command-line processing; no root reached'),
    (0x1005a4, 0x1bc210, 'none', 'after zeroing 0x24e590.. state words'),
    (0x1005b0, 0x1c0930, 'a0="cmndata/2d/" (0x221848)', 'sets loader directory prefix'),
    (0x1005bc, 0x1bc538, 'a0="main.xpf" (0x221858)', 'registers MAIN.XPF archive for later lookup; no root reached'),
    (0x1005c4, 0x1c0930, 'a0=0', 'clears loader directory prefix'),
    (0x1005cc, 0x1bc528, 'a0=0', 'no root reached'),
    (0x1005d4, 0x1cff30, 'a0=0', 'DMA/queue sync'),
    (0x1005e0, 0x114b58, 'none', 'loop head; filter-parameter fade stepping on 0x332934 table only'),
    (0x1005e8, 0x1bfb98, 'none', 'no root reached'),
    (0x1005f0, 0x197518, 'none', 'font cache reset (197670); no root reached'),
    (0x1005f8, 0x1c8ed0, 'none', 'no root reached'),
    (0x100600, 0x1c8f50, 'a0=0x4140', 'returns buffer, stored at 0x1f8480 (100614)'),
    (0x100610, 0x1c9420, 'a0=prev result,a1=0x414', 'no root reached'),
    (0x100618, 0x114f50, 'none', 'FIRST SCHEDULER TRAVERSAL (j 114f70 when *0x1f85c0 != 0)'),
]

EFFECT_NAMES_TABLE = 0x221bf8
GLYPH_PALETTE_SRC = 0x2003f8

def derive_top_calls(elf):
    out = []
    pc = MAIN
    while pc <= FIRST_SCHEDULER_CALL:
        w = elf.w32(pc)
        if w >> 26 == 3:
            out.append((pc, ((w & 0x3ffffff) << 2) | ((pc + 4) & 0xf0000000)))
        pc += 4
    return out

def sha(b):
    return hashlib.sha256(b).hexdigest()

def clut_swizzle(words, n):
    """1cd858 + 1cd928: zero 512-word buffer, entry i -> CSM1 position."""
    buf = [0] * 512
    for i in range(n):
        pos = (((i >> 5) << 1) + ((i & 8) >> 3)) * 16 + (((i & 0x10) >> 4) << 3) + (i & 7)
        buf[pos] = words[i]
    return buf

def clut_upload_bytes(entries_le_words, n):
    height = ((n + 31) >> 5) << 1
    buf = clut_swizzle(entries_le_words, n)
    return b''.join(struct.pack('<I', x) for x in buf[:16 * height]), height

def parse_xim(b):
    q0 = struct.unpack_from('<Q', b, 0)[0]
    psm = (q0 >> 20) & 0x3f
    cpsm = (q0 >> 51) & 0xf
    ext, _x, levels, colors = struct.unpack_from('<4I', b, 0x10)
    ib = 0x10 + ext
    height, width = struct.unpack_from('<2I', b, ib + 8)
    bpp = {19: 8, 20: 4}[psm]
    nbytes = width * height * bpp // 8
    pix = b[ib + 0x10: ib + 0x10 + nbytes]
    pal = [struct.unpack_from('<I', b, 0x20 + 4 * i)[0] for i in range(colors)]
    return dict(psm=psm, cpsm=cpsm, levels=levels, colors=colors, width=width,
                height=height, pixels=pix, palette=pal, size=len(b))

def font_payload(fnt):
    count = fnt[5] - fnt[4] + 1
    ah = 8 + 0x14 * count
    height = fnt[ah] | (fnt[ah + 1] << 8)
    width = fnt[ah + 2] | (fnt[ah + 3] << 8)
    n = (width * height) >> 1
    return dict(count=count, width=width, height=height, offset=ah + 4,
                payload=bytes(fnt[ah + 4: ah + 4 + n]))

class LibcRand:
    """1f14b0: next = next*0x41c64e6d + 0x3039 (u32); return next & 0x7fffffff."""

    def __init__(self, seed):
        self.n = seed
        self.calls = 0

    def __call__(self):
        self.n = (self.n * 0x41c64e6d + 0x3039) & 0xffffffff
        self.calls += 1
        return self.n & 0x7fffffff

def noise_payload(seed, w=0x40, h=0x80):
    """192ea0 (w=0x40,h=0x80): two 256-entry palettes then w*h pixel bytes."""
    r = LibcRand(seed)
    words = [0] * 256
    for i in range(256):                     # 192f10-192f48
        x = 0x80000000
        x |= (r() << 3) & 0xff00
        x |= (r() >> 5) & 0xff
        words[i] = x
    pal_a = list(words)
    for i in range(256):                     # 192f68-192f94
        s = (r() >> 5) & 0xff
        words[i] |= (s << 16) | (s << 8) | s
    pal_b = list(words)
    pix = bytes((r() >> 5) & 0xff for _ in range(w * h))   # 192fd0-193008
    return pal_a, pal_b, pix, r.calls

class Builder:
    def __init__(self):
        self.events = []
        self.allocs = []         # dicts
        self.handles = []
        self.refs = {}
        self.uploads = 0

    def ev(self, kind, step, **kw):
        d = {'seq': len(self.events), 'kind': kind, 'step': step}
        d.update(kw)
        self.events.append(d)
        return d

    def alloc(self, step, pc, via, cls, psm, w, h, purpose):
        aid = 'A%02d' % len(self.allocs)
        self.allocs.append(aid)
        self.refs[aid] = 0
        self.ev('alloc', step, id=aid, pc=pc, via=via, allocator='0x1cf008',
                alloc_class=cls, psm=psm, width=w, height=h, purpose=purpose, initial_references=0)
        return aid

    def ref(self, step, aid, delta, pc, via, reason):
        self.refs[aid] += delta
        self.ev('ref_adjust', step, alloc=aid, delta=delta, pc=pc, via=via, reason=reason,
                references_after=self.refs[aid])

    def handle(self, step, pc, aid, purpose):
        h = len(self.handles)
        self.handles.append(h)
        self.ev('handle_create', step, handle=h, record=h, image_alloc=aid, pc=pc,
                via='0x1cc880 (from 0x1cd1e0)', purpose=purpose,
                handle_id_derivation='hypothesis: first-fit HR allocator, no reservations in prefix',
                record_name='')
        self.ref(step, aid, 1, '0x1cc9dc', '0x1cc880', 'record image descriptor reference')
        return h

    def upload(self, step, pc, aid, psm, w, h, data, source, via, note=''):
        self.uploads += 1
        d = self.ev('upload', step, alloc=aid, pc=pc, via=via, transfer='0x1cba48 -> 0x1c9bb8',
                    x=0, y=0, rrw=w, rrh=h, psm=psm, bytes=len(data), sha256=sha(data), source=source)
        if note:
            d['note'] = note
        return d

    def store(self, step, pc, addr, value, meaning):
        self.ev('global_store', step, pc=pc, address=addr, value=value, meaning=meaning)

def annotate_derivations(events):
    """Attach how each allocation argument is derived (all read from the listed code)."""
    for e in events:
        if e['kind'] != 'alloc':
            continue
        via, st = e['via'], e['step']
        if '0x1cdb80' in via:
            e['arg_derivation'] = ('class 2 = constant at 0x1cdcd4; psm = 1cdb80 a1 = 1cb1e8 a0 (const 0 at 0x1003f8/0x1004bc); '
                                   'width/height = 1cdb80 a2/a3 = 1cb1e8 a1/a2 (const 0x280/0xe0 at 0x100420-0x100424; 18f8c0: 0x18f8cc/0x18f8dc)')
        elif '0x1cb1e8' in via:
            e['arg_derivation'] = ('class 3 = constant at 0x1cb344; psm = 1cb1e8 t0 (const 0x31 at 0x100414); width/height = 1cb1e8 a1/a2')
        elif '0x1cd1e0' in via and st == '1b':
            e['arg_derivation'] = 'psm 0x14 (197398 a1 from 1974f8 constant 0x14 at 0x197504); width 0x400 const 0x197474; height = clamp(0x100 rounded to 32) from 1974f8 a0 (const 0x100 at 0x100494)'
        elif '0x1cd1e0' in via and st == '2':
            e['arg_derivation'] = 'constants a0=0x13 (0x19301c), a1=w=0x40, a2=h=0x80 (192ea0 args from 1943a8 at 0x1943c4/0x1943cc); class 1 constant at 0x1cd210'
        elif '0x1cd1e0' in via and st == '3':
            e['arg_derivation'] = 'psm/width/height read from the XIM member header by 18f6a8 (psm=(u64@0>>20)&0x3f, image block height/width); class 1 constant at 0x1cd210'
        elif '0x1cd1e0' in via and st == '4':
            e['arg_derivation'] = 'psm from flags&0x3000 (0x1000->0x2c, 0x2000->0x24, 0->0x14, 0x194a14-0x194a78); width/height LE16 atlas header after the glyph records; class 1 constant'
        elif '0x1cd7d0' in via:
            e['arg_derivation'] = ('class 0 / psm 0 constants (0x1cd808/0x1cd80c); width 16 constant (0x1cd7dc); '
                                   'height = ((n+31)>>5)<<1 with n = entry count passed by the caller (16 or 256)')

PSMNAME = {0: 'PSMCT32', 19: 'PSMT8', 20: 'PSMT4', 36: 'PSMT4HL', 44: 'PSMT4HH', 49: 'PSMZ24'}

def build_events(elf, local):
    B = Builder()
    import os
    # ---- step 0: allocator and handle system init (pre-100428)
    B.ev('vram_allocator_init', 0, pc='0x1003d8', via='0x1cb088 -> 0x1ce940',
         note='clears occupancy map and 64 per-PSM cursors, seeds 4096 FIFO nodes')
    B.ev('handle_system_init', 0, pc='0x1003d8', via='0x1cb088 -> 0x1cc248 -> 0x1cbdf8(0x200,0x80)',
         records=0x200, extra_palettes=0x80, handle_ids=0x1000, reservation_class=0, palette_mode=0)
    # ---- step 1: 100428 -> 1cb1e8
    s = '1'
    c0 = B.alloc(s, '0x1cdcd0', '0x1cb2ec -> 0x1cdb80', 2, 0, 640, 224, 'colour buffer, layer 0 (0x3a17d0)')
    B.ref(s, c0, 1, '0x1cddd8', '0x1cdb80 -> 0x1cdda0', 'attach colour to layer 0 (+0x100)')
    c1 = B.alloc(s, '0x1cdcd0', '0x1cb30c -> 0x1cdb80', 2, 0, 640, 224, 'colour buffer, layer 1 (0x3a18e0)')
    B.ref(s, c1, 1, '0x1cddd8', '0x1cdb80 -> 0x1cdda0', 'attach colour to layer 1 (+0x100)')
    B.store(s, '0x1cb328', '0x3a19f0', 'desc(' + c0 + ')', 'layer-0 colour descriptor copy')
    B.store(s, '0x1cb334', '0x3a19f8', 'desc(' + c1 + ')', 'layer-1 colour descriptor copy')
    z = B.alloc(s, '0x1cb34c', '0x1cb1e8', 3, 49, 640, 224, 'shared depth buffer PSMZ24')
    B.store(s, '0x1cb364', '0x3a1a00', 'desc(' + z + ')', 'shared depth descriptor')
    B.ref(s, z, 1, '0x1cde50', '0x1cb360 -> 0x1cde18', 'attach depth to layer 0 (+0x108)')
    B.ref(s, z, 1, '0x1cde50', '0x1cb36c -> 0x1cde18', 'attach depth to layer 1 (+0x108)')
    B.ev('global_store', s, pc='0x1cb3b8', address='0x3a1790', value='DISPFB1.FBP=base(' + c1 + ')>>5',
         meaning='display env 0 shows layer 1')
    B.ev('global_store', s, pc='0x1cb3e0', address='0x3a17b8', value='DISPFB1.FBP=base(' + c0 + ')>>5',
         meaning='display env 1 shows layer 0')
    B.store(s, '0x1cb3e8', '0x3a1778', '0', 'current layer index')
    # ---- step 1b: 100490 -> 1974f8 -> 197398
    s = '1b'
    h0img = B.alloc(s, '0x1cd20c', '0x197478 -> 0x1cd1e0', 1, 20, 1024, 256, 'glyph-cache texture (pixels NULL: no upload)')
    h0 = B.handle(s, '0x1cd220', h0img, 'glyph-cache texture handle -> 0x200494')
    B.ev('no_upload', s, alloc=h0img, pc='0x1cd244', reason='pixel pointer a3 == NULL (0x197478 delay slot)')
    pal = parse_palette_glyph(elf)
    cl = B.alloc(s, '0x1cd810', '0x1974b0 -> 0x1cd7d0', 0, 0, 16, 2, '16-entry glyph CLUT (src 0x2003f8)')
    data, hh = clut_upload_bytes(pal, 16)
    B.upload(s, '0x1cd9a8', cl, 0, 16, 2, data, 'ELF .data 0x2003f8 (16 words) swizzled by 0x1cd858', '0x1cd928')
    B.ev('palette_bind', s, handle=h0, level=0, alloc=cl, pc='0x1974bc', via='0x1cd598 -> 0x1cd298')
    B.ref(s, cl, 1, '0x1cd318', '0x1cd298', 'record +8 palette descriptor reference')
    B.store(s, '0x197488', '0x200494', 'handle %d' % h0, 'glyph-cache handle')
    # ---- step 2: 1004b8 -> 18fc68(0)
    s = '2'
    B.store(s, '0x18fcac', '0x332930', '0xffffffff', 'filter flags = ~a0 (a0 = 0)')
    B.ev('host_alloc', s, pc='0x18fc94', size=0x120, stored_at='0x332934', purpose='filter slot table (zeroed later by 0x18faa0)')
    # 1943a8 -> 193470 (mesh, host only) -> 192ea0
    B.ev('host_alloc', s, pc='0x1934a4', size=0xb00, stored_at='0x332a98', purpose='warp mesh vertex buffer (11x8 = 88 entries x 32 B); host only')
    pa, pb, pix, ncalls = noise_payload(elf.w32(0x214ec8 + 0x58))
    npa = alloc_pal(B, s, '0x192f54', 'noise CLUT A (0x332b00)', pa, 'libc rand pass 1', elf)
    npb = alloc_pal(B, s, '0x192fa0', 'noise CLUT B (0x332b08)', pb, 'libc rand pass 2', elf)
    nimg = B.alloc(s, '0x1cd20c', '0x193018 -> 0x1cd1e0', 1, 19, 64, 128, 'noise texture')
    h1 = B.handle(s, '0x1cd220', nimg, 'noise texture handle -> 0x332af8')
    B.upload(s, '0x1cd274', nimg, 19, 64, 128, pix, 'libc rand() stream, 8192 bytes (192fd0-193008), seed word 1', '0x1cd1e0')
    B.ev('palette_bind', s, handle=h1, level=0, alloc=npa, pc='0x19302c', via='0x1cd298')
    B.ref(s, npa, 1, '0x1cd318', '0x1cd298', 'record +8 palette descriptor reference')
    B.ev('palette_bind', s, handle=h1, level=1, alloc=npb, pc='0x193040', via='0x1cd298 extra-palette chain',
         extra_palette_index='0 (hypothesis: first free P node)')
    B.ref(s, npb, 1, '0x1cd434', '0x1cd298', 'extra palette descriptor reference')
    B.store(s, '0x1943d8', '0x332af8', 'handle %d' % h1, 'noise texture handle')
    cap = B.alloc(s, '0x1cdcd0', '0x18f8ec -> 0x1cdb80', 2, 0, 640, 224, 'capture target colour buffer (layer 0x332940)')
    B.ref(s, cap, 1, '0x1cddd8', '0x1cdb80 -> 0x1cdda0', 'attach colour to capture layer (+0x100)')
    B.ref(s, z, 1, '0x1cde50', '0x18f900 -> 0x1cde18', 'attach shared depth to capture layer (+0x108); depth = 0x1cbc80()')
    B.store(s, '0x18f918', '0x33293c', '1', 'capture layer ready')
    f1 = B.alloc(s, '0x1cd810', '0x190f44 -> 0x1cd7d0', 0, 0, 16, 16, 'filter CLUT 1 (0x332a68), source NULL')
    B.ev('no_upload', s, alloc=f1, pc='0x1cd824', reason='1cd7d0 source a1 == NULL; allocation only, never retained, bytes undefined')
    B.store(s, '0x190f54', '0x332a68', 'desc(' + f1 + ')', 'filter CLUT 1 descriptor')
    B.ev('gs_display_list_recorded', s, pc='0x1908b0', list_store='0x332a70',
         reads_alloc=[z, f1], executes=False,
         note='70 sprite quads, 7 rows x 10 pages, texture = depth pages as PSMT8 TBW8 through filter CLUT 1; recorded into heap by 0x1ad1f8..0x1ad240, trimmed via slot 0x21357c; no DMA kick reachable (0x1cad20 not in closure)')
    B.store(s, '0x192e30', '0x332a80', '&layer(0x332940)', 'filter source layer pointer')
    f2 = B.alloc(s, '0x1cd810', '0x19103c -> 0x1cd7d0', 0, 0, 16, 16, 'filter CLUT 2 (0x332a78), source NULL')
    B.ev('no_upload', s, alloc=f2, pc='0x1cd824', reason='1cd7d0 source a1 == NULL; allocation only, never retained, bytes undefined')
    B.store(s, '0x19104c', '0x332a78', 'desc(' + f2 + ')', 'filter CLUT 2 descriptor')
    B.ev('gs_display_list_recorded', s, pc='0x1914d0', list_store='0x332a84',
         reads_alloc=[cap, f2], executes=False,
         note='70 sprite quads texturing the capture colour buffer pages through filter CLUT 2; recorded only')
    B.store(s, '0x192af8', '0x332a88', '&layer(0x332940)', 'filter mode-2 source layer')
    B.store(s, '0x192b00', '0x332a60', '0', 'palette-mode selector (also 0x192e98, 0x18fae8)')
    B.store(s, '0x18fa8c', '0x332a50', '1', 'filter gate (initialised)')
    # ---- step 3: shared effects
    s = '3'
    B.ev('global_store', s, pc='0x1011e0', address='0x339294', value='0x1018a0', meaning='callback slot 1 registration (1018f0 -> 1bc168)')
    B.ev('occupancy_probe', s, pc='0x1011e8', via='0x1cf910', note='read-only VRAM occupancy count before effects')
    names = [elf.cstr(elf.w32(EFFECT_NAMES_TABLE + 4 * i)) for i in range(31)]
    assert elf.w32(EFFECT_NAMES_TABLE + 4 * 31) == 0
    xdir = os.path.join(local, 'xpf-decoded/CMNDATA/2D/ENV_TEX.XPF')
    effects = []
    for i, nm in enumerate(names):
        b = open(os.path.join(xdir, nm), 'rb').read()
        x = parse_xim(b)
        ia = B.alloc(s, '0x1cd20c', '0x18f7ec -> 0x1cd1e0', 1, x['psm'], x['width'], x['height'],
                     'effect %d %s image' % (i, nm))
        hh_ = B.handle(s, '0x1cd220', ia, 'effect %d %s' % (i, nm))
        B.upload(s, '0x1cd274', ia, x['psm'], x['width'], x['height'], x['pixels'],
                 'ENV_TEX.XPF member %s (image block + 0x10, verbatim)' % nm, '0x1cd1e0')
        pa_ = B.alloc(s, '0x1cd810', '0x18f82c -> 0x1cd7d0', 0, 0, 16, ((x['colors'] + 31) >> 5) << 1,
                      'effect %d %s CLUT (%d entries)' % (i, nm, x['colors']))
        data, _hh = clut_upload_bytes(x['palette'], x['colors'])
        B.upload(s, '0x1cd9a8', pa_, 0, 16, ((x['colors'] + 31) >> 5) << 1, data,
                 'ENV_TEX.XPF member %s palette (+0x20, %d words) swizzled by 0x1cd858' % (nm, x['colors']), '0x1cd928')
        B.ev('palette_bind', s, handle=hh_, level=0, alloc=pa_, pc='0x18f88c', via='0x1cd298')
        B.ref(s, pa_, 1, '0x1cd318', '0x1cd298', 'record +8 palette descriptor reference')
        effects.append(dict(index=i, name=nm, psm=x['psm'], cpsm=x['cpsm'], colors=x['colors'],
                            width=x['width'], height=x['height'], file_bytes=x['size'],
                            file_sha256=sha(b), image_sha256=sha(x['pixels'])))
    B.store(s, '0x103890', '0x24e780', 'array of 31 handle ids in authored order (+0x1f8498 counter = 31)', 'effect handle array')
    B.ev('occupancy_probe', s, pc='0x1011fc', via='0x1cf910', note='read-only; difference stored at 0x24e618')
    # ---- step 4: fonts
    s = '4'
    fonts = []
    for idx, (fn, flags, psm) in enumerate((('IQ24.FNT', 0x1000, 44), ('BMUI.FNT', 0x2000, 36), ('BM.FNT', 0, 20))):
        b = open(os.path.join(local, 'font-input/CMNDATA/FONT', fn), 'rb').read()
        fp = font_payload(b)
        ia = B.alloc(s, '0x1cd20c', '0x194a7c -> 0x1cd1e0', 1, psm, fp['width'], fp['height'],
                     'font %d %s atlas (flags 0x%x)' % (idx, fn, flags))
        hh_ = B.handle(s, '0x1cd220', ia, 'font %d %s' % (idx, fn))
        B.upload(s, '0x1cd274', ia, psm, fp['width'], fp['height'], fp['payload'],
                 '%s bytes [%d:%d] verbatim (no repack)' % (fn, fp['offset'], fp['offset'] + len(fp['payload'])), '0x1cd1e0')
        pa_ = B.alloc(s, '0x1cd810', '0x194afc -> 0x1cd7d0', 0, 0, 16, 2, 'font %d CLUT (16 entries, src 0x2003f8)' % idx)
        data, _hh = clut_upload_bytes(pal, 16)
        B.upload(s, '0x1cd9a8', pa_, 0, 16, 2, data, 'ELF .data 0x2003f8 swizzled by 0x1cd858', '0x1cd928')
        B.ev('palette_bind', s, handle=hh_, level=0, alloc=pa_, pc='0x194b08', via='0x1cd598 -> 0x1cd298')
        B.ref(s, pa_, 1, '0x1cd318', '0x1cd298', 'record +8 palette descriptor reference')
        fonts.append(dict(index=idx, file=fn, flags=flags, psm=psm, width=fp['width'], height=fp['height'],
                          glyph_records=fp['count'], payload_offset=fp['offset'], payload_bytes=len(fp['payload']),
                          file_sha256=sha(b), payload_sha256=sha(fp['payload'])))
    return B, dict(effects=effects, fonts=fonts, noise=dict(
        rand_seed_word=elf.w32(0x214ec8 + 0x58), rand_calls=ncalls,
        palette_a_sha256=sha(b''.join(struct.pack('<I', x) for x in pa)),
        palette_b_sha256=sha(b''.join(struct.pack('<I', x) for x in pb)),
        pixels_sha256=sha(pix)))

def parse_palette_glyph(elf):
    return [elf.w32(GLYPH_PALETTE_SRC + 4 * i) for i in range(16)]

def alloc_pal(B, step, pc_site, purpose, words, src, elf):
    a = B.alloc(step, '0x1cd810', pc_site + ' -> 0x1cd7d0', 0, 0, 16, 16, purpose)
    data, _h = clut_upload_bytes(words, 256)
    B.upload(step, '0x1cd9a8', a, 0, 16, 16, data, 'generated: ' + src + ', swizzled by 0x1cd858', '0x1cd928')
    return a

KINDS = {'vram_allocator_init', 'handle_system_init', 'alloc', 'ref_adjust', 'handle_create', 'upload',
         'no_upload', 'palette_bind', 'global_store', 'host_alloc', 'gs_display_list_recorded',
         'occupancy_probe'}

def check(doc):
    """Return a list of problems (empty = valid and internally consistent)."""
    bad = []

    def need(c, m):
        if not c:
            bad.append(m)
    need(doc.get('schema') == SCHEMA, 'schema')
    need(doc.get('elf', {}).get('sha256') == ELF_SHA256, 'elf sha256')
    ev = doc.get('events')
    need(isinstance(ev, list) and ev, 'events missing')
    if not isinstance(ev, list):
        return bad
    refs, allocs, handles, uploaded, bound = {}, {}, set(), {}, set()
    for i, e in enumerate(ev):
        need(e.get('seq') == i, 'seq %d' % i)
        k = e.get('kind')
        need(k in KINDS, 'kind %r' % (k,))
        if k == 'alloc':
            need(e['id'] not in allocs, 'duplicate alloc id ' + e['id'])
            need(e['alloc_class'] in (0, 1, 2, 3), 'class')
            need(e['width'] > 0 and e['height'] > 0, 'dims')
            need(e['psm'] in (0, 19, 20, 36, 44, 49), 'psm %r' % e['psm'])
            allocs[e['id']] = e
            refs[e['id']] = e['initial_references']
        elif k == 'ref_adjust':
            need(e['alloc'] in allocs, 'ref of unknown alloc ' + str(e['alloc']))
            if e['alloc'] in refs:
                refs[e['alloc']] += e['delta']
                need(refs[e['alloc']] == e['references_after'], 'refs_after mismatch seq %d' % i)
                need(refs[e['alloc']] >= 0, 'negative refs')
        elif k == 'handle_create':
            need(e['handle'] == len(handles), 'handle ids not sequential')
            need(e['image_alloc'] in allocs, 'handle image alloc')
            handles.add(e['handle'])
        elif k == 'upload':
            a = allocs.get(e['alloc'])
            need(a is not None, 'upload to unknown alloc')
            if a:
                need((e['rrw'], e['rrh'], e['psm']) == (a['width'], a['height'], a['psm']),
                     'upload rect differs from allocation seq %d' % i)
                need(e['alloc'] not in uploaded, 'second upload to alloc ' + e['alloc'])
                uploaded[e['alloc']] = e
                need(len(e['sha256']) == 64 and e['bytes'] > 0, 'upload hash/bytes')
        elif k == 'palette_bind':
            need(e['handle'] in handles and e['alloc'] in allocs, 'bind refs')
    for aid, e in allocs.items():
        need(refs[aid] == doc.get('final_references', {}).get(aid), 'final_references[%s]' % aid)
    c = doc.get('counts', {})
    need(c.get('allocations') == len(allocs), 'count allocations')
    need(c.get('uploads') == len(uploaded), 'count uploads')
    need(c.get('handles') == len(handles), 'count handles')
    need(c.get('palette_binds') == sum(1 for e in ev if e['kind'] == 'palette_bind'), 'count binds')
    need(c.get('allocations_without_upload', None) == sum(1 for a in allocs if a not in uploaded), 'count unuploaded')
    need(doc.get('unresolved'), 'unresolved list empty')
    tc = doc.get('top_level_calls', [])
    need(tc and tc[0]['pc'] == '0x100320' and tc[-1]['callee'] == '0x114f50', 'top-level call bounds')
    need(all(t['seq'] == i for i, t in enumerate(tc)), 'top-level seq')
    return bad

# ---------------------------------------------------------------------------

UNRESOLVED = [
    ('U01', 'Allocator-derived values (base block, TBW, packed descriptor, available count, cursors, occupancy) are '
            'NOT read from the ELF; the "model" fields come from a replay of the native Ps2TextureMemory and are not '
            'independent source evidence. They must be reproduced by the unchanged original 0x1cf008 before use.'),
    ('U02', 'Indirect calls (jalr) inside the call closures are counted but not resolved. Address-taken functions that '
            'reach a resource root exist (data pointers: 0x18f7a8 at 0x1fe308; scene/jump-table entries) but no startup '
            'call site was shown to dispatch to them other than 0x18f7a8 via 0x186aa8 (not in the prefix).'),
    ('U03', 'Handle ids and record indices (0..35) assume first-fit HR allocation and FIFO record list with no '
            'reservation (0x1cd5c8 is not reachable) and no earlier handle creation; derived by reading, not observed.'),
    ('U04', 'GS-register-level behaviour of the SDK/system init calls (0x1c7730, 0x1c6918, 0x16c1e0, 0x1c8b40, '
            '0x1cff30, 0x1cb1c0) is not decoded; their closures reach DMA/GS helper functions (0x1e1750, 0x1e1840, '
            '0x1e5f48, 0x1e48a0). No VRAM allocator, handle, upload or draw-list root is reachable, but a GS '
            'reset or clear writing VRAM contents is not excluded by this static pass.'),
    ('U05', 'VU1 microprogram uploads (0x1ace90, 0x1b1e60, 0x1795e8) are classified as VIF MPG packets from the '
            'packet structure (0x1afa40) and call shape; the packet bytes were not decoded exhaustively.'),
    ('U06', 'Effect system init inside 0x103a88 (0x1143d8, 0x10bc78: 103d30/103b78/1138c0/113f00/114058/10ba38) was '
            'not read instruction by instruction; no resource root is reachable but GS register/packet writes are '
            'not excluded.'),
    ('U07', 'Allocator slots 0x213574/0x213578/0x21357c/0x213580 are installed at runtime; 0x21357c is read as a '
            'trim/realloc (hypothesis). Host-heap behaviour is irrelevant to VRAM but not resolved.'),
    ('U08', 'The noise texture and both noise CLUTs depend on the libc rand() state. The seed word (1) comes from the '
            'ELF .data image and no srand call site is reachable, and 0x192ea0 is the first rand consumer by static '
            'closure; a hidden earlier rand() call through a jalr is not excluded. The Python replay is derived from '
            'reading, not compared with an original run.'),
    ('U09', 'Filter CLUT 1/2 (0x332a68, 0x332a78) bytes: no write inside the prefix was found (0x1902d8 is reached '
            'only through the 0x2003c8 callback table from the filter dispatcher 0x18f9f8, which runs after the first '
            'scheduler traversal at 0x100628). Colour/depth/capture buffer bytes: no draw, clear or copy that executes '
            'inside the prefix was found; the two recorded display lists (0x332a70, 0x332a84) are not kicked.'),
    ('U10', 'Exact host-to-local packet chunking and the GS state left by uploads (0x1cba48/0x1c9bb8) were read for '
            'argument flow only; PSM20/36/44 qwc = w*h>>5 is from the 0x1c9b48 jump table.'),
    ('U11', 'Failure paths (allocation failure, load failure) were not exercised; the executed contract assumes success, '
            'which the native model confirms for the allocator (no failed allocation in the replay).'),
    ('U12', 'Whether 0x1ed1c0, 0x16c1e0 (and similar leaf/library calls) have effects on texture memory is argued '
            'only by absence of roots in the jal closure.'),
]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--local')
    ap.add_argument('--elf')
    ap.add_argument('--functions-tsv')
    ap.add_argument('--model-jsonl')
    ap.add_argument('--out')
    ap.add_argument('--check')
    a = ap.parse_args()
    if a.check:
        probs = check(json.load(open(a.check)))
        for p in probs:
            print('FAIL', p)
        print('OK' if not probs else '%d problems' % len(probs))
        return 1 if probs else 0
    import os
    local = a.local
    elf = Elf(a.elf or os.path.join(local, 'extracted/SCUS_971.29'))
    if elf.sha256 != ELF_SHA256:
        sys.exit('wrong ELF')
    tsv = a.functions_tsv or os.path.join(local, 'research/material-texture-ownership-v1/initializer-exports/functions.tsv')
    derived = derive_top_calls(elf)
    curated = [(pc, tg) for pc, tg, _a, _n in TOP]
    if derived != curated:
        sys.exit('top-level call table differs from ELF:\n%s\n%s' % (derived, curated))
    g = Graph(elf, tsv)
    top = []
    for i, (pc, tg, args, note) in enumerate(TOP):
        t = {'seq': i, 'pc': hex(pc), 'callee': hex(tg), 'args': args, 'note': note}
        t.update(g.summarize(tg))
        top.append(t)
    B, assets = build_events(elf, local)
    annotate_derivations(B.events)
    assets['elf_glyph_palette_source'] = {
        'address': hex(GLYPH_PALETTE_SRC), 'bytes': 64, 'sha256': sha(elf.rb(GLYPH_PALETTE_SRC, 64)),
        'note': 'ELF .data; read-only in the prefix (no store found); used by 0x1974f8 and all three fonts'}
    assets['env_tex_archive'] = {
        'path': 'local/xpf-packed/CMNDATA/2D/ENV_TEX.XPF',
        'sha256': sha(open(os.path.join(local, 'xpf-packed/CMNDATA/2D/ENV_TEX.XPF'), 'rb').read()),
        'decoded_members_dir': 'local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF/',
        'note': 'mounted by 0x1bccf0("env_tex.xpf") after 0x1c0930("cmndata/2d/"); members resolved by 0x1c2e68(name)'}
    for e in assets['effects']:
        e['path'] = 'local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF/' + e['name']
    for f in assets['fonts']:
        f['path'] = 'local/font-input/CMNDATA/FONT/' + f['file']
        f['runtime_name'] = 'cmndata/font/' + f['file'].lower()
    seed = elf.w32(0x214ec8 + 0x58)
    st = seed
    for _ in range(assets['noise']['rand_calls']):
        st = (st * 0x41c64e6d + 0x3039) & 0xffffffff
    st2 = st
    for _ in range(384):
        st2 = (st2 * 0x41c64e6d + 0x3039) & 0xffffffff
    rng = {'generator': '0x1f14b0 libc rand: next = next*0x41c64e6d+0x3039 (u32); returns next&0x7fffffff; state word at *(0x2151b4)+0x58 = 0x214ec8+0x58',
           'initial_state': seed, 'srand_callers_in_prefix': 0,
           'consumers_in_prefix': [
               {'pc': '0x192ea0', 'draws': assets['noise']['rand_calls'], 'state_before': hex(seed), 'state_after': hex(st),
                'purpose': 'noise texture: 256+256 palette draws then 8192 pixel draws'},
               {'pc': '0x10e098', 'draws': 384, 'state_before': hex(st), 'state_after': hex(st2),
                'purpose': 'effect template noise grid (8*8*6 draws, each rand()%7), reached 1011d0->103a88->10bc78->10e098(8) after the 31 loads'}],
           'native_note': 'Session::random starts at 1 (research precondition); the original reaches the effect-template grid at state ' + hex(st)}
    doc_extra = {'rng': rng}
    doc = {
        'schema': SCHEMA,
        'elf': {'path': 'local/extracted/SCUS_971.29', 'sha256': elf.sha256},
        'boundary': {'entry': '0x100008', 'main': hex(MAIN), 'last_call_before_traversal': hex(FIRST_SCHEDULER_CALL - 4),
                     'first_scheduler_traversal_call': hex(FIRST_SCHEDULER_CALL), 'scheduler_entry': hex(SCHEDULER_ENTRY)},
        'top_level_calls': top,
        'events': B.events,
        'assets': assets,
        'final_references': dict(B.refs),
        'rng': doc_extra['rng'],
        'counts': {
            'allocations': len(B.allocs),
            'uploads': B.uploads,
            'handles': len(B.handles),
            'palette_binds': sum(1 for e in B.events if e['kind'] == 'palette_bind'),
            'allocations_without_upload': sum(1 for a in B.allocs if not any(
                e['kind'] == 'upload' and e['alloc'] == a for e in B.events)),
            'frees_executed': 0,
        },
        'unresolved': [{'id': i, 'text': t} for i, t in UNRESOLVED],
    }
    if a.model_jsonl:
        rows = [json.loads(l) for l in open(a.model_jsonl) if l.strip()]
        allocs = [r for r in rows if r.get('op') == 'A']
        for r, aid in zip(allocs, B.allocs):
            for e in B.events:
                if e['kind'] == 'alloc' and e['id'] == aid:
                    e['model'] = {k: r[k] for k in ('id', 'descriptor', 'base_block', 'tbw')}
                    e['model']['provenance'] = 'native Ps2TextureMemory replay; not independent source evidence'
    out = json.dumps(doc, indent=1, sort_keys=False)
    if a.out:
        open(a.out, 'w').write(out + '\n')
    else:
        print(out)
    return 0

if __name__ == '__main__':
    sys.exit(main())
