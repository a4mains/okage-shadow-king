#!/usr/bin/env python3
"""Generate and verify the Retire title update / menu / retirement source contract.

Window: from the first 0x1296f8 title update (same traversal as the 0x12c350(selector=1)
return that ends the Title contract) through ENTRY to 0x124548 with mode 10 (Restore) or
mode 9 (New Game) selected, for two ordinary input routes.  See the title-retirement input contract.
Pure Python 3, standard library only.  Nothing here executes game code.

  python3 tools/retire_title_contract.py --local /path/to/okage/local --out docs/retire-title-contract.json
  python3 tools/retire_title_contract.py --check docs/retire-title-contract.json

--local checks the ELF hash first, re-verifies every curated call/word/instruction anchor
against the pinned ELF, re-reads the owned TES.EFD effect-106 record and rebuilds the
contract from the tracked Title contract (docs/title-contract.json, hash-pinned) as the
start state.  --check needs only the JSON files: it replays both routes' ordered events
through the Title handle/record/reference models and fails on any inconsistency.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import struct
import sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
_spec = importlib.util.spec_from_file_location('title_contract', os.path.join(HERE, 'title_contract.py'))
title = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(title)

ELF_SHA256 = title.ELF_SHA256
SCHEMA = 'okage.retire.title-contract.v1'
TITLE_PATH = 'docs/title-contract.json'
TITLE_SHA256 = 'c3e3e296bd2eef2a0c4218659318fd61dd17a669574c1a410123dc7bc6a65f50'
TES_EFD = 'effect-input/CMNDATA/2D/TES.EFD'
TES_EFD_SHA256 = '2c99993d5ac51760c426b4b157fb65c1348c85d09ef2552fc47e5241b3e43c6c'
STATUSES = ('proven', 'hypothesis', 'unreached', 'open')
CHECKPOINTS = ('first-update', 'stable-menu', 'removal-request', 'resource-retirement', 'successor-entry')
ROUTES = ('restore', 'new_game')
STARTUP_ALLOCS = ['A%02d' % i for i in range(79)]
STARTUP_HANDLES = list(range(36))

CALL_ANCHORS = [
    # 12bfc8 mode8 tick prefix and selector-1 dispatch
    (0x12c8f8, 'j', 0x12bfc8), (0x12c00c, 'jal', 0x1c01d0), (0x12c03c, 'jal', 0x1c0330), (0x12c1c8, 'jal', 0x114c80),
    (0x12c1d0, 'jal', 0x1c0160), (0x12c25c, 'j', 0x1296f8),
    # 1296f8 title update
    (0x129734, 'jal', 0x1b4220), (0x129754, 'jal', 0x1b4220), (0x1297a4, 'jal', 0x1bb5b8), (0x1297b8, 'jal', 0x1b43c0),
    (0x1297e0, 'jal', 0x1b4220), (0x1297ec, 'jal', 0x1b43c0), (0x129814, 'jal', 0x1b4220), (0x129834, 'jal', 0x1b3710),
    (0x129920, 'jal', 0x114c80), (0x129a3c, 'jal', 0x1b3710), (0x129a6c, 'jal', 0x1b3710), (0x129a9c, 'jal', 0x1b3710),
    (0x129b50, 'jal', 0x1294b0), (0x129b64, 'jal', 0x10c188), (0x129bd4, 'jal', 0x12df80), (0x129c74, 'jal', 0x124f10),
    (0x129c7c, 'jal', 0x1251d8), (0x129e34, 'jal', 0x1b3710), (0x129e54, 'jal', 0x1b3710), (0x129f18, 'jal', 0x12df80),
    (0x129f98, 'jal', 0x12df80), (0x12a064, 'jal', 0x12df80), (0x12a238, 'jal', 0x114c80), (0x12a26c, 'jal', 0x103640),
    (0x12a2ac, 'jal', 0x124f10), (0x12a2b4, 'jal', 0x1251d8), (0x12a2d4, 'jal', 0x124930), (0x12a2e4, 'jal', 0x126788),
    (0x12a2ec, 'jal', 0x1252a8), (0x12a300, 'jal', 0x124930),
    # effect 106 creation / rebinding / removal request
    (0x1294cc, 'jal', 0x101960), (0x1019fc, 'jal', 0x103b78), (0x10c208, 'jal', 0x1cc2b0), (0x10c2e0, 'jal', 0x10bec0),
    (0x10c30c, 'j', 0x10bec0), (0x1036d8, 'j', 0x103640),
    # resource child tick -> effects
    (0x100fb0, 'jal', 0x100800), (0x100ff4, 'jal', 0x1012a8), (0x101000, 'jal', 0x114760), (0x10104c, 'j', 0x198390),
    (0x1012bc, 'jal', 0x102218), (0x1012d0, 'jal', 0x102cf8), (0x1023fc, 'jal', 0x103bf8), (0x102440, 'jal', 0x103bf8),
    (0x1024d0, 'jal', 0x1131d0), (0x102e4c, 'jal', 0x1131d0), (0x103148, 'jal', 0x111f48), (0x111f70, 'jal', 0x1cc2b0),
    (0x111f7c, 'jal', 0x1cd5a8), (0x11322c, 'jal', 0x103640), (0x113254, 'jal', 0x10bb48), (0x10bc04, 'jal', 0x105a40),
    (0x106ac8, 'jal', 0x10c388), (0x106b6c, 'jal', 0x10c978), (0x106ba8, 'jal', 0x10c978), (0x10c9fc, 'jal', 0x10c7b8),
    (0x10c8c4, 'jal', 0x1f14b0), (0x10c8cc, 'jal', 0x1f14b0), (0x10c3fc, 'jal', 0x103b78), (0x1984ec, 'jal', 0x19d090),
    # scheduler, parent, successor construction
    (0x115024, 'jal', 0x114f70), (0x115078, 'jal', 0x114f70), (0x1150cc, 'jal', 0x1bdaf0), (0x1150e8, 'jal', 0x1bdaf0),
    (0x124878, 'jal', 0x1007e0), (0x1248a4, 'jal', 0x1246b0), (0x1248b0, 'jal', 0x1008d8), (0x1247d8, 'jal', 0x1151b8),
    (0x1151d4, 'jal', 0x114e98), (0x124574, 'jal', 0x1151b8), (0x12458c, 'jal', 0x1cb4b8), (0x124598, 'jal', 0x114c80),
    # resource-child destructor (Title anchors re-checked) and the reached model/image paths
    (0x10107c, 'jal', 0x1b5fc8), (0x101090, 'jal', 0x1891b8), (0x101098, 'jal', 0x114530), (0x1010a0, 'jal', 0x101240),
    (0x1010a8, 'jal', 0x198550), (0x1010bc, 'jal', 0x1cca30), (0x1010c4, 'jal', 0x13c368), (0x1010cc, 'jal', 0x1b84b0),
    (0x1010d4, 'jal', 0x1b6f50), (0x1010dc, 'jal', 0x1b5fd0), (0x1010e4, 'jal', 0x1b3fc0), (0x1010ec, 'jal', 0x18fb00),
    (0x1010f4, 'jal', 0x1b8028), (0x101248, 'jal', 0x1036d8), (0x101254, 'j', 0x10d150),
    (0x1b850c, 'jal', 0x186ef8), (0x1b8568, 'jal', 0x1797b8), (0x1b8580, 'jal', 0x1bb758), (0x1b85ac, 'j', 0x1b47c8),
    (0x1b6f60, 'jal', 0x1b6f08), (0x1b6fac, 'j', 0x1b60c0), (0x1b6f34, 'jal', 0x1b6400), (0x1b64a8, 'jal', 0x1797b8),
    (0x1797cc, 'jal', 0x179478), (0x179810, 'jal', 0x179650), (0x179820, 'j', 0x179538), (0x1796ac, 'jal', 0x179558),
    (0x1796f0, 'jal', 0x1cca30), (0x17970c, 'jal', 0x1cfca8), (0x1b60fc, 'jal', 0x1cca30),
    (0x1b6000, 'jal', 0x1b4c18), (0x1b6070, 'jal', 0x1b4b00), (0x1b4b00, 'j', 0x1b2240), (0x1b608c, 'j', 0x1b4a70),
    (0x1b227c, 'jal', 0x1cca30),
    (0x1cca88, 'jal', 0x178a70), (0x1ccb20, 'jal', 0x1cee50), (0x1ccb28, 'jal', 0x1cf728), (0x1ccbd8, 'jal', 0x1cee50),
    (0x1ccbe0, 'jal', 0x1cf728), (0x1ccbf8, 'jal', 0x1cee50), (0x1ccc00, 'jal', 0x1cf728), (0x1cccd0, 'jal', 0x1cfca8),
    (0x1ccd10, 'j', 0x178b18),
    # instance creation (Title constructor) and its single mesh entry
    (0x12c7bc, 'jal', 0x1b8a80), (0x1b8b40, 'jal', 0x1b6e38), (0x1b6ea8, 'jal', 0x1b64d8),
    # mode8 destructor and deferred theme fade
    (0x12c8e8, 'j', 0x12dd50), (0x1006a0, 'jal', 0x12ce20), (0x12ce78, 'jal', 0x1757e0), (0x12ce8c, 'jal', 0x12de98),
    (0x12ce98, 'jal', 0x12dcb0),
]
# Data words (address, expected u32).
WORD_ANCHORS = [
    (0x227654, 0x12c234),                                                 # mode8 update switch: selector 1
    (0x223760 + 4 * 7, 0x1247b0), (0x223760 + 4 * 8, 0x1247c4),           # mode 8, mode 9
    (0x223760 + 4 * 9, 0x1247c4), (0x223760 + 4 * 10, 0x1247e8),          # mode 10, mode 11
    (0x223750, 0x124548), (0x223754, 0), (0x223758, 0x124630), (0x22375c, 0x124628),   # mode 9/10 node descriptor
    (0x227988, 0x12c350), (0x22798c, 0), (0x227990, 0x12c8f8), (0x227994, 0x12c8d8),   # mode8 node descriptor
    (0x221980, 0x100ef8), (0x221984, 0), (0x221988, 0x100f90), (0x22198c, 0x101058),   # resource child descriptor
    (0x221f20 + 4 * 32, 0x106a84), (0x221f20, 0x106bb0),                  # 105a40 timing switch: 32 water, 0 none
    (0x221fb0, 0x108c34), (0x222090, 0x1091e8), (0x2220d0, 0x109bf4), (0x222150, 0x10a7a0),   # other selectors 0
] + [(0x2226a8 + 4 * i, 1 << i) for i in range(16)]                     # key-mask template copied by 115a98
PHASE_TABLE = 0x2272f0
PHASE_CASES = {0: 0x129acc, 10: 0x129ba4, 12: 0x129bf8, 13: 0x129c20, 15: 0x129c90, 20: 0x129d20, 30: 0x129ea0,
               50: 0x12a0ac, 55: 0x12a12c, 57: 0x12a1ac, 60: 0x12a238}
PHASE_DEFAULT = 0x12a308
# Exact instruction words carrying the constants and branch conditions this contract relies on.
INSN_ANCHORS = [
    (0x12c0b4, 0x24041000, 'up mask compare 0x1000 (1c01d0 word & *0x1f85f8)'),
    (0x12c0d8, 0x24032000, 'right mask compare 0x2000 (& *0x1f85fc) -> 0x259968'),
    (0x12c0f4, 0x24034000, 'down mask compare 0x4000 (& *0x1f8600) -> 0x25996c'),
    (0x12c110, 0x34038000, 'left mask compare 0x8000 (& *0x1f8604) -> 0x259970'),
    (0x12c150, 0x24020008, 'direction repeat lockout timer *0x259960 = 8'),
    (0x12c068, 0x2882ff9c, 'analog up: (ly-0x80) < -100'), (0x12c078, 0x28a20065, 'analog right: (lx-0x80) >= 101'),
    (0x12c160, 0x24c2ffe3, 'stick-centred test raw lx-29'), (0x12c164, 0x2c4200c7, 'stick-centred test < 199 (unsigned)'),
    (0x129bb0, 0x30630800, 'phase 10: Start = pressed word & 0x800'),
    (0x129be0, 0x284205dd, 'phase 10 timeout: counter > 1500 -> phase 12 (attract)'),
    (0x129ce4, 0x28420028, 'phase 15: 40 frames'), (0x129e60, 0x28420028, 'phase 20: 40 frames'),
    (0x129e80, 0x24020001, 'phase 20 -> 30: default selection 1 (Restore)'),
    (0x129e84, 0xae420038, 'store selection mode8+0x38'),
    (0x1298fc, 0x3c013b44, 'fade-in step hi (0.003 = 0x3b449ba6)'), (0x129900, 0x34219ba6, 'fade-in step lo'),
    (0x12a244, 0x3c013ccc, 'phase 60 fade step hi (0.025 = 0x3ccccccd)'), (0x12a248, 0x3421cccd, 'phase 60 fade step lo'),
    (0x12a258, 0x46020036, 'phase 60: c.ole.s b8 <= 0 triggers removal + mode request'),
    (0x129af0, 0x2404006a, 'phase 0: effect definition 0x6a = 106'),
    (0x10c1c4, 0x2406006a, '10c188 rebinding accepts only effect 106'),
    (0x103648, 0x2402fffe, '103640(-2): all-slot form'), (0x103654, 0x24030054, '84 effect slots'),
    (0x1036dc, 0x2404fffe, '1036d8 passes -2 (removal request on every live slot)'),
    (0x12953c, 0x2406bfff, '1294b0 clears job flag 0x4000'),
    (0x129b6c, 0xafa0005c, 'phase 0 writes only alpha lane (=0) of the first water colour'),
    (0x129de0, 0x3c01c4af, 'New Game item x = -1400.0'), (0x129dec, 0x3c0144ac, 'Restore item x = 1380.0 hi'),
    (0x129df0, 0x34218000, 'Restore item x lo'), (0x12a0c4, 0x3c014170, 'phase 50 scroll +15.0'),
    (0x12a144, 0x3c014170, 'phase 55 scroll -15.0'),
    (0x129ed4, 0x14440003, 'phase 30 left: selection-1 (clamped at 0)'),
    (0x129f38, 0x28430002, 'phase 30 right: selection+1 (clamped at 1)'),
    (0x12a050, 0x8c4485dc, 'phase 30 confirm mask *0x1f85dc'),
    (0x12a078, 0x24020032, 'selection 0 -> phase 50'), (0x12a08c, 0x24020037, 'selection 1 -> phase 55'),
    (0x12a0a0, 0x24020039, 'selection 2 -> phase 57 (unreached)'),
    (0x12a27c, 0x10620012, 'phase 60: selection 1 -> mode 10'), (0x12a2c4, 0x24050009, 'mode 9 (New Game)'),
    (0x12a2cc, 0x2405000a, 'mode 10 (Restore)'),
    (0x1023b8, 0x30620200, '102218: flag 0x200 -> reclaim (host frees, slot = -1)'),
    (0x1023c0, 0x30620100, '102218: flag 0x100 -> arm 0x200, skip'),
    (0x102468, 0x34620200, '102218: set 0x200'), (0x102478, 0x30624000, '102218: 0x4000 skip'),
    (0x102484, 0x30620400, '102218: 0x400 -> deferred list'),
    (0x113220, 0x30428000, '1131d0: looping 0x8000 avoids self-removal'),
    (0x106aa0, 0x30620020, 'water case: job initialised flag 0x20 selects lazy 10c388'),
    (0x10c84c, 0x10c2000b, '10c7b8: W == 1 deterministic impulse'),
    (0x10c864, 0x10c20013, '10c7b8: W == 2 is the only rand() case'),
    (0x1b8b7c, 0xa6850030, '1b8a80: instance +0x30 = -1'),
    (0x1b8c04, 0x28c20002, '1b8a80: +0x30 replaced only when (flags>>5)&3 >= 2'),
    (0x1b855c, 0x84440030, '1b84b0 reads instance +0x30'), (0x1b8560, 0x10920003, '1b84b0: +0x30 == -1 -> no 1797b8'),
    (0x1b6f28, 0x86030000, '1b6f08 reads instance +0'), (0x1b6f2c, 0x10600005, '1b6f08: +0 == 0 -> skip'),
    (0x1b6520, 0xa6420000, '1b64d8: instance +0 = 0x900'), (0x1b6524, 0xa6510002, '1b64d8: instance +2 = mesh slot'),
    (0x1b6434, 0x1060001c, '1b6400 calls 1797b8 even when the mesh-slot record is empty'),
    (0x1797f0, 0x0205102a, '1797b8: slot < record base -> next'), (0x179808, 0x5040fff5, '1797b8: slot >= base+meshes -> next'),
    (0x1ccaa8, 0x2442ffff, '1cca30: record handle count - 1'), (0x1ccab0, 0x1c60006a, '1cca30: still referenced -> handle-only'),
    (0x1ccabc, 0x04400006, '1cca30: hash slot < 0 -> no hash write'), (0x1ccad4, 0xa4450000, '1cca30: hash[slot] = -1 (tombstone)'),
    (0x12dd5c, 0x24020080, '12dd50: theme fade counter 0x80'), (0x12dd64, 0xace59a60, '12dd50: *0x2d9a60 = 1'),
    (0x12ce5c, 0x2442ffff, '12ce20: per-iteration fade counter - 1'),
    (0x124564, 0x8e070014, '124548 reads its parent'), (0x1245ac, 0x24020009, '124548: mode 9 branch'),
    (0x1245b4, 0x2402000a, '124548: mode 10 branch'),
]
F_FADE_IN = 0x3b449ba6
F_FADE_OUT = 0x3ccccccd
F_ONE = 0x3f800000
X_NEW = 0xc4af0000
X_RESTORE = 0x44ac8000
F_STEP = 0x41700000

def verify_elf(elf):
    bad = []
    if elf.sha256 != ELF_SHA256:
        return ['ELF sha256 %s != %s' % (elf.sha256, ELF_SHA256)]
    for pc, kind, target in CALL_ANCHORS:
        k, t = elf.branch_target(pc)
        if (k, t) != (kind, target):
            bad.append('call anchor %x: %s %s' % (pc, k, hex(t) if t else t))
    for a, v in WORD_ANCHORS:
        if elf.w32(a) != v:
            bad.append('word anchor %x = %x' % (a, elf.w32(a)))
    for p in range(0x3d):
        want = PHASE_CASES.get(p, PHASE_DEFAULT)
        if elf.w32(PHASE_TABLE + 4 * p) != want:
            bad.append('phase table entry %d' % p)
    for pc, w, why in INSN_ANCHORS:
        if elf.w32(pc) != w:
            bad.append('instruction anchor %x (%s) = %08x' % (pc, why, elf.w32(pc)))
    return bad

def f32(bits):
    return Fraction(struct.unpack('<f', struct.pack('<I', bits))[0])

def f32_bits(x):
    return struct.unpack('<I', struct.pack('<f', float(x)))[0]

def round_f32(x, toward_zero=True):
    """Exact rational -> float32 (normal range).  toward_zero=False: IEEE nearest-even."""
    if x == 0:
        return Fraction(0)
    s = -1 if x < 0 else 1
    a = abs(x)
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    elif Fraction(2) ** (e + 1) <= a:
        e += 1
    step = Fraction(2) ** (e - 23)
    q, r = divmod(a, step)
    q = int(q)
    if not toward_zero and (r * 2 > step or (r * 2 == step and q & 1)):
        q += 1
    return s * q * step

def fade_in_frames(toward_zero=True):
    """mode8+0xb8 from 0.0: frames until 1296f8 clamps it to 1.0 (0x1298e4..0x12991c)."""
    b, c, one, n = Fraction(0), f32(F_FADE_IN), Fraction(1), 0
    while b < one:
        n += 1
        f = round_f32(b + c, toward_zero)
        b = one if f > one else f
    return n

def fade_value_after(frames, toward_zero=True):
    b, c, one = Fraction(0), f32(F_FADE_IN), Fraction(1)
    for _ in range(frames):
        if b < one:
            f = round_f32(b + c, toward_zero)
            b = one if f > one else f
    return b

def phase60_frames(b8, toward_zero=True):
    """Phase 60 (0x12a238): 114c80(b8); b8 -= 0.025; request on the frame b8 <= 0."""
    b, c, n = Fraction(b8), f32(F_FADE_OUT), 0
    while True:
        n += 1
        b = round_f32(b - c, toward_zero)
        if b <= 0:
            return n

def scroll_frames(route):
    """Phase 55: x(+0x50)=1380 -= 15 until x < 0.  Phase 50: x(+0x4c)=-1400 += 15 until 0 < x."""
    x, n = (f32(X_RESTORE), 0) if route == 'restore' else (f32(X_NEW), 0)
    step = f32(F_STEP)
    while True:
        n += 1
        if route == 'restore':
            x = round_f32(x - step)
            if x < 0:
                return n
        else:
            x = round_f32(x + step)
            if x > 0:
                return n

def timeline(route, start, confirm):
    """Frame numbers (1 = first 1296f8) for given Start/confirm frames.  [H: R5900 round-toward-zero]"""
    if not 2 <= start <= 1502:
        raise ValueError('Start must be observed on phase-10 frames 2..1502 (counter 1..1501)')
    if confirm < start + 81:
        raise ValueError('confirm is consumed only in phase 30 (from frame start+81)')
    b8 = fade_value_after(confirm)
    m60 = phase60_frames(b8)
    n = confirm + scroll_frames(route) + m60
    return {'first_update': 1, 'start': start, 'phase15': [start + 1, start + 40], 'phase20': [start + 41, start + 80],
            'stable_menu': start + 81, 'confirm': confirm,
            'scroll': [confirm + 1, confirm + scroll_frames(route)], 'phase60': [n - m60 + 1, n],
            'b8_at_phase60_bits': '0x%08x' % f32_bits(b8), 'removal_request': n, 'destructors': n + 1,
            'node_frees': n + 2, 'successor_entry': n + 3}

def load_title(path):
    with open(path, 'rb') as f:
        b = f.read()
    return b, json.loads(b)

def replay_title(doc):
    """Replay the Title events through the Title models to recover exact list orders at its stop."""
    init = doc['initial_state']
    model = title.HandleModel(init['live_handles_in_order'], init['handle_records'])
    refs = dict(init['live_allocations'])
    info = {}
    pal = {}
    for e in doc['events']:
        k = e['kind']
        if k == 'reserve':
            model.reserve(e['count'], e['reserved_class'])
        elif k == 'handle_create':
            h, r = model.create(e['reserved_class'])
            assert (h, r) == (e['handle'], e['record'])
            info[h] = {'record': r, 'image_alloc': e['image_alloc'], 'name': e['name'], 'palettes': []}
        elif k == 'palette_bind':
            info[e['handle']]['palettes'].append(e['alloc'])
        elif k == 'handle_retire':
            model.retire(e['handle'])
        elif k == 'alloc':
            refs[e['id']] = 0
        elif k == 'ref_adjust':
            refs[e['alloc']] += e['delta']
    fs = doc['final_state']
    freed = set(fs['freed_allocations'])
    assert {a: r for a, r in refs.items() if a not in freed} == fs['references']
    assert model.in_use == fs['live_handles_in_order']
    slot_of = {rec8 - 8: s for s, rec8 in fs['hash_entries']}
    return model, {a: r for a, r in refs.items() if a not in freed}, info, slot_of

class Builder:
    def __init__(self, title_doc):
        self.model, self.refs, self.info, self.slot_of = replay_title(title_doc)
        self.hash = dict(self.slot_of)
        self.tombstones = []
        self.events = []
        self.checkpoint = None
        self.freed = []

    def ev(self, kind, status, pc, **kw):
        e = {'seq': len(self.events), 'checkpoint': self.checkpoint, 'kind': kind, 'status': status, 'pc': pc}
        e.update(kw)
        self.events.append(e)
        return e

    def mark(self, name, frame, note):
        self.checkpoint = name
        self.ev('checkpoint', 'proven', '-', name=name, frame=frame, note=note)

    def retire(self, h, pc_call, via):
        """0x1cca30(h) for a handle that is the only handle of its record (cache disabled; Title)."""
        inf = self.info[h]
        r, last = self.model.retire(h)
        E = self.ev
        E('handle_retire', 'hypothesis', pc_call, handle=h, record=r, last_handle=last, name=inf['name'], via=via,
          note='ids derived (Title U06); record handle count 1 -> 0 at 0x1ccaa8')
        if not last:
            raise AssertionError('title handles are the only handle of their record')
        slot = self.hash.pop(r, -1)
        if slot >= 0:
            self.tombstones.append(slot)
            E('hash_tombstone', 'proven', '0x1ccad4', record=r, slot=slot, value=-1,
              note='record+0x24 >= 0: hash[slot] = -1; an older duplicate already lost its slot (0x1cc6d4 set its +0x24 to -1)')
        else:
            E('hash_untouched', 'proven', '0x1ccabc', record=r,
              note='record+0x24 == -1 (no name, or an older duplicate displaced by a newer one)')
        img = inf['image_alloc']
        self.refs[img] -= 1
        E('ref_adjust', 'proven', '0x1ccbd8', alloc=img, delta=-1, references_after=self.refs[img],
          reason='record image descriptor release')
        if self.refs[img] < 1:
            del self.refs[img]
            self.freed.append(img)
            E('free', 'proven', '0x1ccbe0', alloc=img, via='0x1cf728 (refcount 0)')
        if inf['palettes']:
            for pal in inf['palettes']:
                self.refs[pal] -= 1
                E('ref_adjust', 'proven', '0x1ccbf8', alloc=pal, delta=-1, references_after=self.refs[pal],
                  reason='record+8 level-0 CLUT descriptor release')
                if self.refs[pal] < 1:
                    del self.refs[pal]
                    self.freed.append(pal)
                    E('free', 'proven', '0x1ccc00', alloc=pal, via='0x1cf728 (refcount 0)')
        else:
            E('null_descriptor', 'proven', '0x1ccbf8', record=r,
              note='PSM1 record+8 == 0 (0x1cc9c0): 1cee50(0,-1) returns 0 at 0x1cee90, 1cf728(0) returns at 0x1cf760')
        E('record_release', 'hypothesis', '0x1ccc10', record=r,
          note='record node r+8 -> tail of the free record list (sentinel 7); fields left stale')
        E('handle_id_free', 'hypothesis', '0x1cccd0', handle=h,
          note='handle node h+8 -> free tail, map[h] = -1 (0x1ccca8), HR bit free (0x1cfca8), live count - 1')

def build_route(route, title_doc, efd, fade):
    B = Builder(title_doc)
    E = B.ev
    mode = 10 if route == 'restore' else 9
    sel = 1 if route == 'restore' else 0
    hanten = title_doc['final_state']['mode8_0xcc_handle']
    binding = title_doc['binding']['handles']
    image_list = title_doc['final_state']['image_list']

    # ---------------------------------------------------------------- first update
    B.mark('first-update', 'F1', 'same traversal as the 0x12c350(1) return (Title stop); frame 1 of this contract')
    E('return', 'proven', '0x11528c', note='0x12c350 returns into 0x1151b8 -> 0x1246b0 -> 0x1248ac')
    E('store', 'proven', '0x1248ac', field='root+0x3c', value=0, note='request cleared')
    E('call', 'proven', '0x1248b0', callee='0x1008d8', resource='none (log: 0x1cf910/0x1cc2a0 reads, prints)')
    E('store', 'proven', '0x1248b4', field='root+0x38', value='mode8 node', note='child pointer; then phase 1 -> 0')
    E('traverse_children', 'proven', '0x115018', node='root',
      note='children are read after the root callbacks, so the new mode8 node is ticked in this same traversal')
    E('tick', 'proven', '0x12c8f8', node='mode8', callee='0x12bfc8',
      prefix='0x1c01d0(0) repeat word, clear 0x259964..0x259970, 0x1c0330 analog bytes, repeat timer 0x259960, '
             'direction flags, +0x3c fade (+0x40 rate) -> 0x114c80, +0x28 = 0x1c0160(0) pressed word',
      dispatch='switch +0x24 via 0x227650[1] = 0x12c234: j 0x1296f8')
    E('title_update', 'proven', '0x1296f8', phase=0, note='camera 1b4220(0/1), model channels 1bb5b8/1b43c0/1b4220, '
      'image 0 1b3710(alpha b8), b8 0 -> 0.003, 114c80(b8); no colour write (phase 0 at entry)')
    E('effect_create', 'proven', '0x1294cc', definition=106, via='0x129b50 0x1294b0 -> 0x101960',
      slot='first free of 84 slots in *0x1f849c (expected 0 [H])',
      host_memory='job+elements via 0x103b78 (*0x213574) and 0x103c38 (*0x21357c): host heap, no VRAM',
      copies='template *(*0x1f849c+0x150+4*106): header 0x80 (+0x50 parameters), element 0x80, packet header 0x60',
      then='1294b0: job+0x10 position (0.2,6.5,0), +0x20 rotation (0.25,0,0), +0x30 scale (6,1,0.45) [W lanes unknown]; '
           'flags &= ~0x4000 (0x12953c)')
    E('store', 'proven', '0x129b60', field='mode8+0xc8', value='effect slot')
    E('effect_bind_texture', 'proven', '0x129b64', via='0x10c188(slot, element 0, mode8+0xcc)', handle=hanten,
      handle_source='mode8+0xcc = 0x1b36c0(image record 7) at 0x12c4a8 = 0x337508 list[7] (Title)',
      reads=['0x10c208 0x1cc2b0(handle): image descriptor (read only)'],
      writes={'element+0x5c': 'handle (s16)', 'element+0x6c': '(desc>>38 & 0xfff) - 1', 'element+0x6e': '(desc>>50 & 0xfff) - 1',
              'element+0x68/+0x6a': 0},
      grid='element+0x7c == 0 (template element initialised by 0x112070 at 0x1120a0; only other writer 0x10c388) -> '
           '0x10c248 skips the UV grid and both 0x10bec0 calls',
      refcount='none: no 1cee50/1cc880/1cca30 in the 0x10c188 closure; the effect does not own the handle')
    E('effect_color', 'proven', '0x129b98', lanes='RGB unknown (stack 0x50..0x58 unwritten), A = 0 (0x129b6c)')
    E('resource_tick', 'proven', '0x100f90', note='resource+0x24 = 1 -> 0x100800 image submission; +0x3c = 1, +0x38 = 0 '
      '(0x100ef8; setters 0x100ac0/0x100b20/0x100c08 unreachable) -> 0x1b9be0 3D pass, 0x100d40, 0x1012a8, 0x114760 '
      '(aux ids -1: no upload)')
    E('effect_queue', 'proven', '0x102484', slot='water', note='0x102218: flags 0x8400 & ~0x4000 -> 0x400 deferred list '
      '0x24e630[*0x1f8494++]; not updated by the ordinary loop')
    E('effect_update', 'proven', '0x102e4c', via='0x102cf8 -> 0x1131d0 -> 0x10bb48 -> 0x105a40',
      note='job callback +0x68 (jalr 0x1024c8/0x102cf8) first if non-zero [open: template value]; timing selector '
           '(program>>24)&63 = 32 -> 0x106a84 (water); flag 0x20 clear -> 0x10c388 lazy water buffer (host 0x4020 bytes '
           'via 0x103b78, element+0x7c at 0x10c668), then 0x10c978 -> 0x10c7b8(W=1): deterministic impulse, no rand; '
           '0x10bb48 sets job flag 0x20 (0x10bc50)')
    E('effect_draw', 'proven', '0x103148', reads=['0x111f48 -> 0x1cc2b0(element+0x5c)', '0x1cd5a8 -> 0x1cd500 palette'],
      note='TEX0 = descriptor fields | 0x20000006a8000000 (TCC1, TFX MODULATE); first pass job alpha 0 x authored 0.4 '
           '= 0 -> ATST GREATER 0 / AFAIL KEEP rejects every fragment; RGB still unknown in the packet')
    E('no_resource_op', 'proven', '-', scope='first-update',
      note='no allocation, upload, handle/record create or retire, refcount change, name/hash, reservation, image-list '
           'or free in the 1296f8 / 12bfc8-prefix / 100f90 closures (texture roots only behind the guards listed in '
           'title_update_closures)')

    # ---------------------------------------------------------------- stable menu
    B.mark('stable-menu', 'S+81', 'first phase-30 frame (selection = 1 stored at 0x129e84 on frame S+80)')
    E('phase', 'proven', '0x129ba4', phase=10, frames='2..S', note='counter +0x30; Start (+0x28 & 0x800) -> phase 15, '
      '+0x80 = 1.0, 12df80(3); counter > 1500 without Start -> phase 12 (attract, unreached by these routes)')
    E('sound', 'proven', '0x129bd4', callee='0x12df80', effect=3, note='Start accepted')
    E('phase', 'proven', '0x129c90', phase=15, frames='S+1..S+40')
    E('phase', 'proven', '0x129d20', phase=20, frames='S+41..S+80', note='items 4/5 submitted; S+80: selection +0x38 = 1, '
      'pulse channel +0xc4 = 2 -> phase 30')
    E('effect_color', 'proven', '0x1298a0', frames='2..', lanes='(0.9*b8, 0.9*b8, 0.9*b8, 0.75*b8) with b8 read before '
      'this frame\'s increment; job+0x74 = 2 every frame (0x1298d0)')
    E('no_resource_op', 'proven', '-', scope='frames 2..S+81', note='same closures; water updated/drawn each frame')

    # ---------------------------------------------------------------- menu input + removal request
    B.mark('removal-request', 'N', 'phase-60 frame on which b8 <= 0 (timeline)')
    if route == 'new_game':
        E('pad', 'proven', '0x129ea8', direction='left', flag='0x259970',
          source='1c01d0(0) repeat word & 0x8000 (*0x1f8604) or raw lx < 28, only when *0x259960 == 0',
          effect='selection 1 -> 0 (0x129eb8); +0xc4 = 1; 12df80(7)')
        E('sound', 'proven', '0x129f18', callee='0x12df80', effect=7, note='cursor moved')
    E('pad', 'proven', '0x12a054', button='confirm', mask='*0x1f85dc = 0x40 (0x100384)', word='mode8+0x28 pressed',
      selection=sel, next_phase=55 if sel == 1 else 50)
    E('sound', 'proven', '0x12a064', callee='0x12df80', effect=3, note='confirm')
    E('phase', 'proven', '0x12a12c' if sel == 1 else '0x12a0ac', phase=55 if sel == 1 else 50,
      frames='C+1..C+%d' % scroll_frames(route), note='item scroll 15.0 per frame; b8 frozen (phase >= 50)')
    E('phase', 'proven', '0x12a238', phase=60, frames='N-m60+1..N', note='114c80(b8) then b8 -= 0.025')
    E('effect_removal_request', 'proven', '0x12a26c', via='0x103640(mode8+0xc8)', flag_or=0x100, store='0x103690',
      note='request only: the job, its host buffers, its slot and element+0x5c stay')
    if route == 'new_game':
        E('call', 'proven', '0x12a2ac', callee='0x124f10', resource='host: zero game-state block *0x24f788 (0xd78 bytes) '
          'and fields; no texture/RNG root in closure')
        E('call', 'proven', '0x12a2b4', callee='0x1251d8', resource='host: 0x126370, 0x125e10(0,0x83,0xae,0xf0); no '
          'texture/RNG root in closure')
    E('mode_request', 'proven', '0x12a2d4', via='0x124930(root, mode, 0, 0)', mode=mode, selector=0,
      stores='root+0x3c = mode, +0x48 = 0, +0x50 = 0')
    E('resource_tick', 'proven', '0x100f90', note='same traversal, after the mode8 callback')
    E('effect_removal_armed', 'proven', '0x102468', flag_or=0x200,
      note='0x102218 sees 0x100 without 0x200: sets 0x200 and skips (not queued, not updated, not drawn)')
    E('no_resource_op', 'proven', '-', scope='removal-request frame', note='no texture-resource operation')

    # ---------------------------------------------------------------- retirement
    B.mark('resource-retirement', 'N+1..N+2', 'dead marking + descendant destructors (N+1); wrapper frees (N+2)')
    E('parent', 'proven', '0x124868', phase=0, action='request && child -> child flags |= 0x80000000, root+0x38 = 0')
    E('mark_dead', 'proven', '0x115034', node='mode8', note='first dead pass: flags ^= 0x40000000, children marked dead, '
      'recurse (0x115078), then dtor (0x11509c): descendants first; no callbacks run')
    E('destructor', 'proven', '0x11509c', node='resource 0x221980', dtor='0x101058')
    E('call', 'proven', '0x101074', callee='0x1b5fc8', taken=False, reason='resource+0x20 == -1 (setter 0x100968 unreachable)')
    E('call', 'proven', '0x101088', callee='0x1891b8', taken=False, reason='resource+0x34 == 0 (setter 0x100b20 unreachable)')
    E('call', 'proven', '0x101098', callee='0x114530', taken=True, resource='none: aux texture ids -1 (setter 0x1145c0 unreachable)')
    E('effect_removal_request', 'proven', '0x101248', via='0x101240 -> 0x1036d8 -> 0x103640(-2)', flag_or=0x100,
      note='every non-empty slot of 84 gets 0x100 again; water already 0x100|0x200; no reclamation here')
    E('call', 'proven', '0x101254', callee='0x10d150', resource='0x10ce58 retires 0x24e9a8[0..*0x24e9f8): count 0 '
      '(only appender 0x10cdf0 unreachable)')
    E('call', 'proven', '0x1010a8', callee='0x198550', resource='none (closure)')
    E('call', 'proven', '0x1010b0', callee='0x1cca30', taken=False, reason='resource+0x48 == -1 (setter 0x100c60 unreachable)')
    E('call', 'proven', '0x1010c4', callee='0x13c368', resource='none (closure)')
    E('call', 'proven', '0x1010cc', callee='0x1b84b0', taken=True,
      resource='186ef8 x *0x337ef0 (host table clear; count 0 or 1, data dependent [open]); instance loop: the title '
               'instance has +0x30 = -1 (0x1b8b7c; 0x1b8c40 needs (flags>>5)&3 >= 2, flags 0 at 0x12c7b4) so 0x1b8568 '
               '1797b8 is NOT reached; *0x337b6c = 0, *0x337b70 = 0x280; 0x1bb758 host free of *0x337df0; '
               '0x1b47c8(*0x337d8c) clears a channel entry')
    E('call', 'proven', '0x1010d4', callee='0x1b6f50', taken=True, resource='instance table 0..0x27f via 0x1b6f08')
    E('instance_retire', 'hypothesis', '0x1b6f34', instance='entry s3 of *0x21301c (0 expected [H])', mesh_slot='fp (NIGHT_V '
      'binding base)', note='entry +0 = 0x900 (0x1b6520) != 0 -> 0x1b6400(+2 = mesh slot); then +0 = 0 (0x1b6f3c)')
    E('host_free', 'proven', '0x1b6468', what='mesh-slot record *0x213018+fp*0x14: +0x10[i], +0x10, +4 via *0x213580 '
      '(only if record+0 != 0)')
    E('call', 'proven', '0x1b64a8', callee='0x1797b8', args='mesh slot fp',
      resource='lock 0x179478; walk 0x330144 list: first record with base <= fp < base+meshes = NIGHT_V (list head)')
    E('call', 'proven', '0x179810', callee='0x179650', args='address of the link to the NIGHT_V binding record')
    E('mesh_reset', 'proven', '0x1796ac', callee='0x179558', mesh_slots=1, note='host packet record reset')
    for i, h in enumerate(binding):
        E('binding_retire', 'proven', '0x1796f0', index=i, handle=h, via='0x179650 -> 0x1cca30(record.handles[i])')
        B.retire(h, '0x1796f0', 'binding index %d' % i)
    E('mesh_slot_free', 'proven', '0x17970c', via='0x1cfca8(*0x330140, fp)', count=1)
    E('binding_unlink', 'proven', '0x179718', note='*link = record->next: NIGHT_V record removed; the two startup '
      'embedded-model records stay (Title U10)')
    E('host_free', 'proven', '0x179720', what='binding record via *0x213580')
    E('call', 'proven', '0x179820', callee='0x179538', resource='unlock')
    E('instance_clear', 'proven', '0x1b6f88', note='mesh-slot records 0..127 +0 = 0; then j 0x1b60c0')
    E('call', 'proven', '0x1b6fac', callee='0x1b60c0', resource='XTA list 0x337950[0..*0x337948): count 0 (appender '
      '0x1b6710 unreachable)')
    E('call', 'proven', '0x1010dc', callee='0x1b5fd0', taken=True,
      resource='0x1b4c18 x *0x337610 channel entries (host), host frees 0x337620[0..*0x337920), 0x1b4b00 -> 0x1b2240')
    for i, h in enumerate(image_list):
        E('image_list_retire', 'proven', '0x1b227c', index=i, handle=h, via='0x1b2240 -> 0x1cca30 (insertion order)')
        B.retire(h, '0x1b227c', 'image list index %d' % i)
    E('image_list_reset', 'proven', '0x1b2294', count_before=len(image_list), note='*0x337608 = 0; image records '
      '*0x33749c stay stale')
    E('call', 'proven', '0x1b608c', callee='0x1b4a70', resource='host channel reset')
    E('call', 'proven', '0x1010e4', callee='0x1b3fc0', resource='none')
    E('call', 'proven', '0x1010ec', callee='0x18fb00', resource='filter slots 0,3..8 cleared')
    E('call', 'proven', '0x1010f4', callee='0x1b8028', resource='camera/instance defaults: 0x337ef0/0x337df0/0x337b6c '
      'reset, 0x1b5d10/0x1b5f88 host channels; no texture root')
    E('destructor', 'proven', '0x11509c', node='mode8', dtor='0x12c8d8')
    E('audio_request', 'proven', '0x12c8e8', via='0x12dd50(1)', stores='*0x2d9a60 = 1, *0x2d9a68 = 0x80, *0x2d9a64 = 1',
      note='deferred theme fade: 0x12ce20 (main loop 0x1006a0) decrements the counter once per iteration, sets the '
           'stream volume (0x1757e0) and only at 0 calls 0x12de98 and 0x12dcb0(1)')
    E('parent', 'proven', '0x1248c0', phase=0, action='N+2: request && no child -> root phase 1 (0x1248c0)')
    E('node_free', 'proven', '0x1150e8', node='resource 0x221980', note='second dead pass: recurse first, unlink, '
      '0x1bdaf0 (only child of mode8: no next sibling)')
    E('node_free', 'proven', '0x1150cc/0x1150e8', node='mode8', note='unlink, 0x1bdaf0 (0x1150cc if mode8 has a next '
      'sibling under root, else 0x1150e8 [open O09]); scheduler node count *0x1f85c4 - 1 each')

    # ---------------------------------------------------------------- successor entry
    B.mark('successor-entry', 'N+3', 'STOP at entry of 0x124548')
    E('parent', 'proven', '0x124878', phase=1, action='0x1007e0(0) (sb 0 to 0x24e598); root+0x40 = mode, 0x24f738 = mode, '
      '+0x44 = +0x48 (0), +0x4c = +0x50 (0)', mode=mode)
    E('construct', 'proven', '0x1247c4', mode=mode, via='0x1246b0 table 0x223760[%d] = 0x1247c4' % (mode - 1),
      stores='root+0x4c = mode', node_descriptor='0x223750', node_bytes=0x38)
    E('node_alloc', 'proven', '0x1151d4', via='0x114e98(0x38) (zero-filled host block)', link='last child of root',
      fields='+0 cb0 0, +4 cb1 0x124630, +8 dtor 0x124628, +0x14 root, +0x1c root flags & 0x18000000')
    E('stop', 'proven', '0x124548', mode=mode, note='entry of the successor constructor (jalr at 0x115284); its body '
      '(0x221980 resource child, 0x1cb4b8, 0x114c80, 0x12d470, 0x1243c8, %s 0x11f5f8) has NOT executed'
      % ('0x11b2f0(1),' if mode == 10 else ''))

    return B

def counts_of(B):
    k = lambda kind: sum(1 for e in B.events if e['kind'] == kind)
    return {'events': len(B.events), 'handles_retired': k('handle_retire'), 'records_destroyed': k('record_release'),
            'frees': k('free'), 'ref_adjusts': k('ref_adjust'), 'hash_tombstones': k('hash_tombstone'),
            'null_descriptors': k('null_descriptor'), 'binding_retires': k('binding_retire'),
            'image_list_retires': k('image_list_retire'), 'allocations': 0, 'uploads': 0, 'handles_created': 0,
            'reservations': 0, 'names_registered': 0, 'rng_draws': 0}

FRAME_ORDER = [
    ('0x11528c', 'proven', '0x12c350(1) returns into 0x1151b8 (Title stop)'),
    ('0x1247e0', 'proven', '0x1246b0 returns the mode8 node'),
    ('0x1248ac', 'proven', 'root+0x3c = 0; +0x38 = node (delay slot of 0x1248b0)'),
    ('0x1248b0', 'proven', '0x1008d8 log (reads only; tail 0x12e018)'),
    ('0x1248c0', 'proven', 'root phase 1 -> 0; return to 0x115000 (root cb1 jalr)'),
    ('0x115018', 'proven', 'root children traversal (child list read after callbacks)'),
    ('0x114fdc', 'proven', 'mode8 cb0 = 0 (descriptor 0x22798c): skipped'),
    ('0x115000', 'proven', 'mode8 cb1 0x12c8f8 -> j 0x12bfc8 -> 0x12c234 -> j 0x1296f8: FIRST TITLE UPDATE'),
    ('0x115024', 'proven', 'mode8 children: resource child cb1 0x100f90 (title-before-resource order)'),
]
PER_FRAME = [
    ('0x1005e0..0x100610', 'main loop prefix (Title main_loop_per_iteration)'),
    ('0x100618', 'scheduler traversal 0x114f50 -> 0x114f70(root)'),
    ('0x124988', 'root cb1 -> 0x124818: phase 0, no request -> nothing'),
    ('0x12c8f8', 'mode8 cb1 -> 0x12bfc8 prefix: pad words, direction flags, generic fade, +0x28 pressed word'),
    ('0x12c25c', 'selector 1 -> j 0x1296f8 (title update; switch on +0x2c via 0x2272f0)'),
    ('0x100f90', 'resource child: 0x100800 images, 0x1b9be0 3D model pass, 0x100d40, 0x1012a8 effects '
                 '(0x102218 ordinary, 0x102cf8 deferred), 0x114760 aux (ids -1), 0x1980f8/0x198390 windows (count 0)'),
    ('0x100628..0x1006bc', 'main loop tail: filters, text, vsync, layers/clear (W1), frame list (W2), 0x12ce20 sound'),
]
CLOSURES = [
    {'entry': '0x1296f8 + 12bfc8 prefix (0x1c01d0, 0x1c0330, 0x114c80, 0x1c0160)', 'functions': 133,
     'texture_roots': [], 'rand_sites': [], 'jalr': '0x1c29d8 0x103b78 0x103c38 0x175398 0x1f6110 0x1f66a8 0x1f07e8 (host '
     'allocator slots, sound, libc) [U02]'},
    {'entry': '0x100f90 resource tick', 'functions': 470,
     'texture_roots': [
         {'root': '0x1c9bb8 upload', 'path': '0x100f90 > 0x114760 > 0x1afb48 > 0x1c9bb8',
          'guard': 'aux ids 0x24ea94/0x24ea90 = -1; setter 0x1145c0 unreachable from 1296f8/100f90/12c350/101058'},
         {'root': '0x1cca30 retire', 'path': '0x100f90 > 0x198390 > 0x19d090 > 0x19eec8 > 0x1a5990 > 0x1951e8 > 0x1cca30',
          'guard': '0x198390 returns at 0x1983b4 when *0x2006b0 <= 0; 0x197fc0 (from 0x100ef8) zeroes it; writers '
                   '0x198708/0x198e38/0x199458 unreachable'}],
     'reads_only': ['0x1cc2b0 descriptor', '0x1cd5a8/0x1cd500 palette', '0x17bda8/0x17be28 model binding'],
     'host_memory': ['0x103b78/0x103c38/0x103bf8 (effect jobs, water buffers)']},
    {'entry': 'main loop per-iteration functions (Title list)', 'texture_roots': ['0x1cad20 DMA kick (W1/W2 only)'],
     'rand_sites': []},
    {'entry': '0x124818 root tick, 0x114f70 traversal, 0x1151b8 node alloc, 0x12c8d8, 0x12dd50, 0x124f10, 0x1251d8, '
              '0x12df80, 0x12ce20', 'texture_roots': [], 'rand_sites': []},
    {'entry': '0x101058 destructor', 'functions': 209,
     'texture_roots': [{'root': '0x1cca30', 'sites': ['0x1010bc (skipped, +0x48 = -1)', '0x10ce94 (count 0)',
                                                       '0x1796f0 (reached via 0x1b6f50, not via 0x1b84b0)',
                                                       '0x1b227c (reached)', '0x1b60fc (count 0)']},
                       {'root': '0x1cf728 free', 'sites': ['0x1ccb28 (no extra palettes)', '0x1ccbe0', '0x1ccc00']}],
     'rand_sites': []},
]
RNG = {
    'generator': '0x1f14b0 (libc rand; one Session stream)',
    'draws_reached': [],
    'expected_draws': 0,
    'expected_state': 'unchanged from the first update to the stop (after the RNG startup prefix the reference state is '
                      '0x412c25e1 after 9504 draws; Title window draws are Title U11)',
    'sites_in_closure_not_reached': [
        {'sites': '0x10c8c4, 0x10c8cc (0x10c7b8)', 'status': 'unreached',
         'reason': 'water mode W = (int)job param.w = 1 (TES.EFD record 106 parameters (32,32,3,1)); only W == 2 '
                   '(0x10c864) draws'},
        {'sites': '54 sites in 0x105a40 and 0x103f88/0x1042c8/0x104438/0x1046c8', 'status': 'unreached',
         'reason': 'effect 106 element program 0x20000000: timing selector 32 -> 0x106a84 (no rand); selectors 12/6/18/0 '
                   'are 0 -> 0x108c34/0x1091e8/0x109bf4/0x10a7a0 (no rand); element flags 0x4050 do not change the '
                   'program (0x10a7e4..0x10a858); change time 1.1 > t in [0,1]'},
        {'sites': '0x19c74c, 0x19cad4, 0x19cc78', 'status': 'unreached',
         'reason': '0x198390 window loop: *0x2006b0 == 0 (0x197fc0); no window creator reachable'},
    ],
    'not_in_any_closure': ['main loop', '12bfc8/1296f8', '101058', '124818', '114f70', '1151b8', '12ce20', '124f10',
                           '1251d8', '12df80', 'threads 0x1784c0 0x16d918 0x1c5340 0x1d4b78 0x1e9920 0x178408'],
    'caveats': ['U02 indirect calls', 'H: job+0x50 parameters and +0x68 callback come from the prepared template copy'],
}
MENU = {
    'pad_words': {
        'repeat': '0x1c01d0(0) = u16 at pad record 0x339588+0xa (port 0) -> directions via masks 0x1f85f8..0x1f8604 '
                  '(= 0x1000 up, 0x2000 right, 0x4000 down, 0x8000 left; copied from 0x2226a8 by 0x115a98)',
        'analog': '0x1c0330 copies bytes +0x32/+0x33/+0x30/+0x31 into 0x259974/0x259978/0x25997c/0x259980; '
                  '0x259974/0x259978 are stored back as value-0x80',
        'pressed': '0x1c0160(0) = u16 at +6 (rising edges) -> mode8+0x28 (0x12c1dc) before dispatch',
        'confirm_mask': '*0x1f85dc = 0x40 (0x100384, after 0x115a98 at 0x100360)', 'start_bit': '0x800 literal (0x129bb0)'},
    'direction_flags': {'0x259964': 'up', '0x259968': 'right', '0x25996c': 'down', '0x259970': 'left',
                        'repeat_lockout': '*0x259960 = 8 after any flag; while non-zero it decrements and no flag is '
                                          'computed; a centred stick (raw lx 29..227, |ly-0x80| < 100) zeroes it'},
    'phases': [
        {'phase': 0, 'case': '0x129acc', 'frames': '1', 'action': 'create water (106), bind hanten, alpha 0; -> 10'},
        {'phase': 10, 'case': '0x129ba4', 'frames': '2..S', 'action': 'Start -> 15 (12df80(3)); counter > 1500 -> 12'},
        {'phase': 12, 'case': '0x129bf8', 'status': 'unreached', 'action': 'fade, then mode 11 (attract)'},
        {'phase': 13, 'case': '0x129c20', 'status': 'unreached', 'action': '124f10, 1251d8, mode 9 (no writer of 13 found in 1296f8)'},
        {'phase': 15, 'case': '0x129c90', 'frames': 'S+1..S+40'},
        {'phase': 20, 'case': '0x129d20', 'frames': 'S+41..S+80', 'action': 'selection = 1 (Restore default)'},
        {'phase': 30, 'case': '0x129ea0', 'frames': 'S+81..C', 'action': 'left: selection-1 (min 0), right: '
         'selection+1 (max 1), each 12df80(7); up/down not consumed; confirm -> 50 (sel 0) / 55 (sel 1) / 57 (sel 2)'},
        {'phase': 50, 'case': '0x12a0ac', 'frames': 'C+1..C+94', 'action': 'New Game scroll'},
        {'phase': 55, 'case': '0x12a12c', 'frames': 'C+1..C+93', 'action': 'Restore scroll'},
        {'phase': 57, 'case': '0x12a1ac', 'status': 'unreached', 'action': 'selection 2 impossible (clamped to 0..1)'},
        {'phase': 60, 'case': '0x12a238', 'frames': 'm60 frames', 'action': 'fade out; on b8 <= 0: 103640, '
         '[124f10, 1251d8,] 124930(mode)'}],
    'routes': {'restore': 'Start (phase 10), then confirm with the default selection 1 -> mode 10',
               'new_game': 'Start, then LEFT (not down) to selection 0, then confirm -> mode 9'},
}
SURVIVING_TAILS = [
    {'what': 'effect 106 job', 'status': 'proven', 'state': 'slot still occupied, flags |= 0x100|0x200|0x20, host job and '
     'water buffer (+0x7c) still allocated, element+0x5c = retired hanten handle id (stale, unreferenced)',
     'reclaimed_by': '0x102218 at 0x1023c4..0x102460 (host frees 0x103bf8, slot = -1) on the first ordinary effect '
                     'pass after the stop (successor resource tick) [H]; no texture operation'},
    {'what': 'title theme stream', 'status': 'hypothesis', 'state': 'fade active (*0x2d9a60 = 1), counter 0x80 minus '
     'the 0x12ce20 iterations after the dtor (126 at entry to 0x124548 if one main-loop pass per traversal)',
     'reclaimed_by': '0x12ce20 at counter 0: 0x12de98, 0x12dcb0(1)'},
    {'what': 'sound effects 12df80(3)/(7)', 'status': 'open', 'state': 'one-shot voices; completion not modeled'},
    {'what': 'binding list', 'status': 'proven', 'state': 'two startup embedded-model records remain (Title U10)'},
    {'what': 'image records *0x33749c', 'status': 'proven', 'state': 'stale rows (handles 36..43) not cleared'},
]
OPEN = [
    ('O01', 'Handle ids / record indices (Title U06 continued).', 'hook 0x1cca30 entry: a0 (handle), s3 after 0x1cca7c '
     '(record), 0x1ccad4 v0/a1 (hash slot write); expect 44..110 then 36..43'),
    ('O02', 'Allocator placement of the 83 frees (Title U01) and the free list after them.', 'hook 0x1cf728 entry a0 '
     '(64-bit descriptor) and 0x1cf770 v0 (refcount); dump allocator state at 0x124548'),
    ('O03', 'Effect slot index and template words: slot (expected 0), job+0x50 parameters, job+0x68 callback, template '
     'element+0x7c.', 'hook 0x1294d4 v0; read *(*0x1f849c+4*slot)+0x50..0x5f and +0x68; 0x10c248 s1 (must be 0); '
     '0x10c7b8 entry a2 (must be 1)'),
    ('O04', 'RNG draws: none expected.', 'hook 0x1f14b0 entry with ra; any hit between the first 0x1296f8 and '
     '0x124548 is a contract violation'),
    ('O05', 'Instance index and fields of the title model.', 'hook 0x1b8d74 v0 (s3); read *0x21301c+s3*0x3c +0/+2/+0x30; '
     'hook 0x1b6f34 a0 (mesh slot) and 0x179810 (found record)'),
    ('O06', '0x337ef0 count (186ef8 loop) and 0x337610/0x337920 channel counts at retirement.', 'read at 0x1b84c0 v0, '
     '0x1b5fe0 v0, 0x1b6018 v0'),
    ('O07', 'Frame numbers under real FPU rounding (b8 saturation and phase-60 length).', 'log mode8+0x2c/+0x30/+0x38/'
     '+0xb8 at 0x1296f8 entry every frame, and 0x12a26c hit frame'),
    ('O08', 'Effect reclamation and theme-stream end are after the stop.', 'hook 0x102440 (job free), 0x12ce8c '
     '(theme end); confirm both occur after 0x124548'),
    ('O09', 'Other root children (ordering of the successor link).', 'walk *0x1f85c0+0x18 at 0x124548'),
    ('O10', 'Glyph-cache transfers during title frames (Title U05).', 'hook 0x1976f0'),
    ('O11', 'Indirect calls (Title U02): host allocator slots 0x213574/0x21357c/0x213580, job callback +0x68, sound '
     '0x175398, scheduler jalr.', 'log jalr targets at 0x1024c8, 0x103ba0, 0x103c20, 0x103c68'),
    ('O12', 'Water colour first-pass RGB is unknown source data.', 'read job+0x40..0x4c at 0x102e4c on frame 1'),
]

def efd_record(b):
    """TES.EFD: 'EFD ' + count; records at 16: +0 id, +4 duration, +8 flags, +16 n, +32 parameters (4 f32), then
    n x 116-byte elements (+0 flags, +8 shape, +12 primitive, +16 channels, +20 program, +24 texture, +28 change)."""
    w = lambda p: struct.unpack_from('<I', b, p)[0]
    if w(0) != 0x20444645:
        raise ValueError('EFD header')
    p = 16
    for _ in range(w(4)):
        n = w(p + 16)
        if w(p) == 106:
            a = p + 48
            return {'offset': '0x%x' % p, 'id': 106, 'duration': w(p + 4), 'flags': '0x%x' % w(p + 8), 'elements': n,
                    'parameters': list(struct.unpack_from('<4f', b, p + 32)),
                    'element0': {'flags': '0x%x' % w(a), 'shape': w(a + 8), 'primitive': '0x%x' % w(a + 12),
                                 'channels': '0x%x' % w(a + 16), 'program': '0x%08x' % w(a + 20),
                                 'texture': w(a + 24), 'change_time': struct.unpack_from('<f', b, a + 28)[0],
                                 'interval': list(struct.unpack_from('<4f', b, a + 100))}}
        p += 48 + n * 116
    raise ValueError('effect 106 missing')

def efd_requirements(r):
    """Facts the RNG/alpha conclusions depend on."""
    bad = []
    if r['parameters'][3] != 1.0:
        bad.append('water mode W != 1: 10c7b8 may draw rand()')
    if r['flags'] != '0x8400':
        bad.append('effect flags (looping 0x8000 + deferred 0x400 expected)')
    e = r['element0']
    if e['program'] != '0x20000000':
        bad.append('program selectors')
    if int(e['flags'], 16) & 0xf00:
        bad.append('element flags 0x100..0x800 rewrite the program')
    if not e['change_time'] > 1.0:
        bad.append('program change reachable')
    if r['elements'] != 1:
        bad.append('element count')
    return bad

KINDS = {'checkpoint', 'return', 'store', 'call', 'traverse_children', 'tick', 'title_update', 'effect_create',
         'effect_bind_texture', 'effect_color', 'resource_tick', 'effect_queue', 'effect_update', 'effect_draw',
         'no_resource_op', 'phase', 'sound', 'pad', 'effect_removal_request', 'mode_request', 'effect_removal_armed',
         'parent', 'mark_dead', 'destructor', 'instance_retire', 'host_free', 'mesh_reset', 'binding_retire',
         'handle_retire', 'hash_tombstone', 'hash_untouched', 'ref_adjust', 'free', 'null_descriptor', 'record_release',
         'handle_id_free', 'mesh_slot_free', 'binding_unlink', 'instance_clear', 'image_list_retire', 'image_list_reset',
         'audio_request', 'node_free', 'construct', 'node_alloc', 'stop'}
FORBIDDEN_BEFORE_RETIREMENT = {'handle_retire', 'free', 'ref_adjust', 'hash_tombstone', 'record_release', 'handle_id_free'}

def check(doc, title_doc=None):
    bad = []

    def need(c, m):
        if not c:
            bad.append(m)
    need(doc.get('schema') == SCHEMA, 'schema')
    need(doc.get('elf', {}).get('sha256') == ELF_SHA256, 'elf sha256')
    if title_doc is None:
        p = os.path.join(ROOT, TITLE_PATH)
        with open(p, 'rb') as f:
            tb = f.read()
        need(hashlib.sha256(tb).hexdigest() == doc.get('baseline', {}).get('title_sha256'), 'title baseline hash')
        title_doc = json.loads(tb)
    need(title.check(title_doc) == [], 'title baseline invalid')
    routes = doc.get('routes', {})
    need(sorted(routes) == sorted(ROUTES), 'routes')
    binding = title_doc['binding']['handles']
    image_list = title_doc['final_state']['image_list']
    hanten = title_doc['final_state']['mode8_0xcc_handle']
    finals = []
    for name in ROUTES:
        r = routes.get(name, {})
        ev = r.get('events', [])
        need(ev, '%s: events' % name)
        if not ev:
            continue
        mode = 10 if name == 'restore' else 9
        need(r.get('mode') == mode, '%s: mode' % name)
        model, refs, info, slot_of = replay_title(title_doc)
        hashs = dict(slot_of)
        freed, tomb = [], []
        cps = [e['name'] for e in ev if e['kind'] == 'checkpoint']
        need(cps == list(CHECKPOINTS), '%s: checkpoint order %r' % (name, cps))
        need(ev[-1]['kind'] == 'stop' and ev[-1]['pc'] == '0x124548' and ev[-1].get('mode') == mode,
             '%s: must end at 0x124548 entry with mode %d' % (name, mode))
        need(sum(1 for e in ev if e['kind'] == 'stop') == 1, '%s: one stop' % name)
        cp = None
        bind_seen, list_seen = [], []
        pending = None
        order = {}
        for i, e in enumerate(ev):
            need(e.get('seq') == i, '%s: seq %d' % (name, i))
            k = e.get('kind')
            need(k in KINDS, '%s: kind %r' % (name, k))
            need(e.get('status') in STATUSES, '%s: status at %d' % (name, i))
            need(isinstance(e.get('pc'), str), '%s: pc at %d' % (name, i))
            if k == 'checkpoint':
                cp = e['name']
            need(e.get('checkpoint') == cp, '%s: checkpoint label at %d' % (name, i))
            if k in FORBIDDEN_BEFORE_RETIREMENT:
                need(cp == 'resource-retirement', '%s: resource operation outside retirement at %d' % (name, i))
            order.setdefault(k, i)
            if k == 'effect_bind_texture':
                need(e['handle'] == hanten, '%s: water must bind the hanten handle' % name)
            elif k == 'binding_retire':
                bind_seen.append(e['handle'])
                need(not list_seen, '%s: model binding retires before the image list' % name)
            elif k == 'image_list_retire':
                list_seen.append(e['handle'])
            elif k == 'handle_retire':
                h = e['handle']
                need(h in model.map, '%s: retire of dead handle %s' % (name, h))
                if h in model.map:
                    rr, last = model.retire(h)
                    need((rr, last) == (e['record'], e['last_handle']), '%s: retire record %s' % (name, h))
                    pending = (h, rr, info[h])
            elif k in ('hash_tombstone', 'hash_untouched'):
                need(pending is not None and e['record'] == pending[1], '%s: hash step record' % name)
                s = hashs.pop(e['record'], -1)
                if k == 'hash_tombstone':
                    need(s == e['slot'] and s >= 0, '%s: tombstone slot for record %s' % (name, e['record']))
                    tomb.append(s)
                else:
                    need(s == -1, '%s: record %s holds a hash slot' % (name, e['record']))
            elif k == 'ref_adjust':
                a = e['alloc']
                need(a in refs and a not in freed, '%s: ref of unknown/freed %s' % (name, a))
                need(pending is not None and a in [pending[2]['image_alloc']] + pending[2]['palettes'],
                     '%s: ref_adjust %s not owned by the retiring record' % (name, a))
                if a in refs:
                    refs[a] += e['delta']
                    need(refs[a] == e['references_after'] and refs[a] >= 0, '%s: references_after %s' % (name, a))
            elif k == 'free':
                a = e['alloc']
                need(refs.get(a, 1) == 0 and a not in freed, '%s: free of referenced/freed %s' % (name, a))
                freed.append(a)
                refs.pop(a, None)
            elif k == 'null_descriptor':
                need(pending is not None and not pending[2]['palettes'], '%s: null CLUT on a paletted record' % name)
            elif k == 'record_release':
                need(pending is not None and e['record'] == pending[1], '%s: record release' % name)
            elif k == 'handle_id_free':
                need(pending is not None and e['handle'] == pending[0], '%s: handle id free' % name)
                pending = None
        need(pending is None, '%s: unfinished retirement' % name)
        need(bind_seen == binding, '%s: binding retire order must be table order handles[0..66]' % name)
        need(list_seen == image_list, '%s: image list retire must be insertion order' % name)
        for a, b2 in (('effect_create', 'effect_bind_texture'), ('effect_bind_texture', 'effect_update'),
                      ('effect_removal_request', 'mode_request'), ('mode_request', 'effect_removal_armed'),
                      ('effect_removal_armed', 'mark_dead'), ('mark_dead', 'binding_retire'),
                      ('binding_retire', 'image_list_retire'), ('image_list_reset', 'audio_request'),
                      ('audio_request', 'node_free'), ('node_free', 'construct'), ('construct', 'stop')):
            need(a in order and b2 in order and order[a] < order[b2], '%s: order %s < %s' % (name, a, b2))
        dt = [e['node'] for e in ev if e['kind'] == 'destructor']
        need(dt == ['resource 0x221980', 'mode8'], '%s: destructor order' % name)
        nf = [e['node'] for e in ev if e['kind'] == 'node_free']
        need(nf == ['resource 0x221980', 'mode8'], '%s: wrapper free order' % name)
        mr = [e for e in ev if e['kind'] == 'mode_request']
        need(len(mr) == 1 and mr[0]['mode'] == mode, '%s: one mode request' % name)
        resets = [e['callee'] for e in ev if e['kind'] == 'call' and e.get('callee') in ('0x124f10', '0x1251d8')]
        need(resets == (['0x124f10', '0x1251d8'] if mode == 9 else []), '%s: New Game resets' % name)
        if mode == 9:
            reset_at = [i for i, e in enumerate(ev) if e['kind'] == 'call' and e.get('callee') == '0x124f10']
            need(reset_at and order['effect_removal_request'] < reset_at[0] < order['mode_request'],
                 '%s: resets between the removal request and the mode request' % name)
            lefts = [e for e in ev if e['kind'] == 'pad' and e.get('direction') == 'left']
            need(len(lefts) == 1, '%s: left selects New Game' % name)
        need([e['selection'] for e in ev if e['kind'] == 'pad' and e.get('button') == 'confirm'] ==
             [1 if mode == 10 else 0], '%s: confirm selection' % name)
        # final state
        fs = r.get('final_state', {})
        need(fs.get('references') == refs, '%s: final references' % name)
        need(fs.get('freed_allocations') == freed, '%s: freed order' % name)
        need(fs.get('live_handles_in_order') == model.in_use, '%s: live handles' % name)
        need(fs.get('free_record_tail') == model.free_records[-75:], '%s: free record tail' % name)
        need(fs.get('hash_tombstones') == tomb, '%s: tombstones' % name)
        need(fs.get('hash_live_entries') == sorted([[s, rr + 8] for rr, s in hashs.items()]), '%s: hash live' % name)
        need(sorted(refs) == STARTUP_ALLOCS, '%s: survivors must be exactly the startup allocations' % name)
        need(model.in_use == STARTUP_HANDLES, '%s: live handles must be exactly startup handles 0..35' % name)
        need(fs.get('image_list') == [] and fs.get('reserved_queue_0') == [] and fs.get('reserved_queue_1') == [],
             '%s: lists' % name)
        c = r.get('counts', {})
        k = lambda kind: sum(1 for e in ev if e['kind'] == kind)
        need(c.get('events') == len(ev) and c.get('frees') == len(freed) == k('free'), '%s: counts' % name)
        need(c.get('handles_retired') == k('handle_retire') == len(binding) + len(image_list), '%s: retired' % name)
        need(c.get('rng_draws') == 0 and c.get('allocations') == 0 and c.get('uploads') == 0, '%s: no draws/allocs' % name)
        finals.append(fs)
    if len(finals) == 2:
        need(finals[0] == finals[1], 'both routes must end in the same resource state')
    rng = doc.get('rng', {})
    need(rng.get('draws_reached') == [] and rng.get('expected_draws') == 0, 'rng draws')
    tl = doc.get('timeline', {})
    need(tl.get('scroll_frames') == {'restore': 93, 'new_game': 94}, 'scroll frames')
    need(doc.get('open') and doc.get('unresolved_inherited'), 'open lists')
    efd = doc.get('assets', {}).get('tes_efd_106')
    need(isinstance(efd, dict) and not efd_requirements(efd), 'EFD effect 106 facts')
    return bad

# ---------------------------------------------------------------------------

def find_local(arg):
    if arg:
        return arg
    raise SystemExit('--local is required')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--local')
    ap.add_argument('--elf')
    ap.add_argument('--title', default=os.path.join(ROOT, TITLE_PATH))
    ap.add_argument('--out')
    ap.add_argument('--check')
    a = ap.parse_args()
    if a.check:
        with open(a.check) as f:
            probs = check(json.load(f))
        for p in probs:
            print('FAIL', p)
        print('OK' if not probs else '%d problems' % len(probs))
        return 1 if probs else 0
    local = find_local(a.local)
    elf_path = a.elf or os.path.join(local, 'extracted/SCUS_971.29')
    elf = title.Elf(elf_path)
    if elf.sha256 != ELF_SHA256:
        sys.exit('ELF hash mismatch: %s' % elf.sha256)
    bad = verify_elf(elf)
    if bad:
        sys.exit('ELF anchors failed:\n' + '\n'.join(bad))
    tb, tdoc = load_title(a.title)
    if hashlib.sha256(tb).hexdigest() != TITLE_SHA256:
        sys.exit('Title contract hash changed: %s' % hashlib.sha256(tb).hexdigest())
    with open(os.path.join(local, TES_EFD), 'rb') as f:
        eb = f.read()
    if hashlib.sha256(eb).hexdigest() != TES_EFD_SHA256:
        sys.exit('TES.EFD hash mismatch')
    efd = efd_record(eb)
    eb_bad = efd_requirements(efd)
    if eb_bad:
        sys.exit('TES.EFD effect 106 facts changed: ' + ', '.join(eb_bad))
    fade = {'fade_in_saturation_frame': fade_in_frames(True), 'fade_in_saturation_frame_if_nearest': fade_in_frames(False),
            'phase60_frames_from_one': phase60_frames(1, True), 'phase60_frames_from_one_if_nearest': phase60_frames(1, False)}
    routes = {}
    for name in ROUTES:
        B = build_route(name, tdoc, efd, fade)
        routes[name] = {
            'mode': 10 if name == 'restore' else 9,
            'inputs': MENU['routes'][name],
            'events': B.events,
            'final_state': {
                'references': dict(sorted(B.refs.items())), 'freed_allocations': B.freed,
                'live_handles_in_order': list(B.model.in_use), 'free_record_tail': B.model.free_records[-75:],
                'hash_tombstones': B.tombstones, 'hash_live_entries': sorted([[s, r + 8] for r, s in B.hash.items()]),
                'image_list': [], 'reserved_queue_0': list(B.model.queue[0]), 'reserved_queue_1': list(B.model.queue[1])},
            'counts': counts_of(B),
            'example_timeline': timeline(name, 300, 400),
        }
    tf = tdoc['final_state']
    doc = {
        'schema': SCHEMA,
        'elf': {'path': 'local/extracted/SCUS_971.29', 'sha256': elf.sha256},
        'boundary': {'start': 'first 0x1296f8 (0x12c25c), same traversal as the 0x12c350(1) return that ends Title',
                     'stop': 'entry of 0x124548 (jalr 0x115284 from 0x1151b8 <- 0x1247c4 <- 0x124818 phase 1) with '
                             'root+0x40 = 0x24f738 = root+0x4c = mode (10 Restore, 9 New Game); body not executed'},
        'baseline': {'title_contract': TITLE_PATH, 'title_sha256': hashlib.sha256(tb).hexdigest(),
                     'live_allocations': len(tf['references']), 'live_handles': len(tf['live_handles_in_order']),
                     'image_list': tf['image_list'], 'binding_handles': len(tdoc['binding']['handles']),
                     'hanten_handle': tf['mode8_0xcc_handle'], 'hash_entries': len(tf['hash_entries'])},
        'frame_order_to_first_update': [{'pc': p, 'status': s, 'note': n} for p, s, n in FRAME_ORDER],
        'per_frame_dispatch': [{'pc': p, 'note': n} for p, n in PER_FRAME],
        'title_update_closures': CLOSURES,
        'effect106': {
            'definition': 106, 'creation': '0x129b50 0x1294b0 -> 0x101960 (phase 0, frame 1)',
            'binding': {'table': 'effect slot table *0x1f849c (84 slots), element 0 +0x5c (s16 handle)',
                        'handle': tf['mode8_0xcc_handle'], 'source': 'mode8+0xcc = 0x1b36c0(7) = image record 7 = '
                        'list 0x337508[7] (hanten.xim, Title)', 'kind': 'descriptor-only rebinding: 0x1cc2b0 read, no '
                        'reference count, no allocation, no release of the template handle'},
            'update': 'deferred (flag 0x400) via 0x102cf8 -> 0x1131d0; looping (0x8000); water W=1 deterministic',
            'alpha': 'frame 1: RGB unknown, A = 0 -> all fragments rejected (ATST GREATER 0, MODULATE); from frame 2: '
                     '(0.9, 0.9, 0.9, 0.75) x b8 of the previous frame',
            'removal': 'request 0x12a26c (flag 0x100), armed 0x102468 (0x200) same frame, re-requested 0x101248 '
                       '(103640(-2)); reclamation (0x102218 0x1023c4..0x102460) after the stop',
            'efd': efd},
        'menu': MENU,
        'rng': RNG,
        'timeline': {'unit': 'frame = one 0x1296f8 call = one main-loop iteration; frame 1 = first update',
                     'start_window': 'S in 2..1502', 'stable_menu': 'S+81', 'confirm': 'C >= S+81',
                     'scroll_frames': {'restore': scroll_frames('restore'), 'new_game': scroll_frames('new_game')},
                     'removal_request': 'N = C + scroll + m60(b8 after C increments)', 'destructors': 'N+1',
                     'node_frees': 'N+2', 'successor_entry': 'N+3',
                     'fpu': fade, 'status': 'hypothesis (R5900 round-toward-zero single precision)'},
        'routes': routes,
        'surviving_tails': SURVIVING_TAILS,
        'startup_survivors': {'allocations': STARTUP_ALLOCS[0] + '..' + STARTUP_ALLOCS[-1],
                              'handles': '0..35 (records 0..35)', 'note': 'every Title title allocation A83..A165 is freed; '
                              'A79..A82 were freed in Title; startup reference counts unchanged'},
        'assets': {'tes_efd': TES_EFD, 'tes_efd_sha256': TES_EFD_SHA256, 'tes_efd_106': efd},
        'open': [{'id': i, 'question': q, 'hook': h} for i, q, h in OPEN],
        'unresolved_inherited': ['Title U01 allocator placement', 'Title U02 indirect calls', 'Title U04 GS register bits',
                                 'Title U05 glyph cache', 'Title U06 derived ids', 'Title U10 startup binding records'],
    }
    probs = check(doc, tdoc)
    if probs:
        sys.exit('generated contract fails its own check:\n' + '\n'.join(probs))
    out = json.dumps(doc, indent=1)
    if a.out:
        with open(a.out, 'w') as f:
            f.write(out + '\n')
    else:
        print(out)
    return 0

if __name__ == '__main__':
    sys.exit(main())
