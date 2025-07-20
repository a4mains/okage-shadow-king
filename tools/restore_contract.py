#!/usr/bin/env python3
"""Generate and verify the Restore empty-Restore cancellation / second-title source contract.

Window: from ENTRY of 0x124548 with mode 10 (the Retire stop, same traversal) through the RETURN of the
second 0x12c350(selector=1) into 0x11528c, before its first 0x1296f8 update.  One ordinary route: the
memory card has no save data (all three slots empty), the ready "no saved data" UI is shown, an
ordinary Cancel closes the manager, the wrapper requests mode 8 selector 1, and the title is rebuilt
on the allocator/handle/hash/free-list state left by Retire retirement plus Restore.
See the Restore input contract.  Pure Python 3, standard library only.  Nothing here executes game code.

  python3 tools/restore_contract.py --local /path/to/okage/local --out docs/restore-contract.json
  python3 tools/restore_contract.py --check docs/restore-contract.json

--local checks the ELF hash first, re-verifies every curated call/word/string/instruction anchor
(Title, Retire and Restore sets), re-reads the owned MAIN.XPF/bm_if.xim and the Title SCR/NIGHT_V assets and
rebuilds the contract from the hash-pinned Title and Retire contracts.  --check needs only the JSON files:
it replays the ordered events through the Title handle/record/reservation/name-hash models and the
descriptor reference counts, starting from the Retire Restore-route final state, and fails on any
inconsistency.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

def _load(name, fn):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fn))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m

retire = _load('retire_title_contract', 'retire_title_contract.py')
title = retire.title

ELF_SHA256 = title.ELF_SHA256
SCHEMA = 'okage.restore-contract.v1'
TITLE_PATH = 'docs/title-contract.json'
TITLE_SHA256 = retire.TITLE_SHA256
RETIRE_PATH = 'docs/retire-title-contract.json'
RETIRE_SHA256 = 'dbfedfb97675d393fd23f292460cbe1814279b7d045f846a01c99c42ea2aa081'
BM_IF = 'xpf-decoded/CMNDATA/2D/MAIN.XPF/bm_if.xim'
BM_IF_SHA256 = 'bde86c61981b2785a557502b9235941815f02b0d3a8067d096aa89f975d1129f'
BM_IF_UPLOADS = [('158b046bd8461d276e5231c5a1ecd99acbba3975730645d50fe7c4eaf962b911', 81920),   # PSM19 image
                 ('3997f1b9e0cc6ce79cbdb23e09eaf7f33e77b7a9730f46c8f38395413305c8bb', 1024)]    # swizzled CLUT
MAIN_XPF_PACKED_SHA256 = '57c1f4fc83ad2a24a51cd664def4f5a83d6407606241a1174c9ee38c9a667456'
STATUSES = ('proven', 'hypothesis', 'unreached', 'open')
CHECKPOINTS = ('retire-successor-entry', 'restore-constructor-return', 'first-layout', 'empty-ready', 'cancel-death',
               'resource-retirement', 'wrapper-free', 'second-title-return')
STARTUP_HANDLES = list(range(36))
# Resource operations allowed only inside these checkpoints (everything else must be free of them).
RESOURCE_KINDS = {'alloc', 'handle_create', 'ref_adjust', 'upload', 'palette_bind', 'handle_retire', 'free',
                  'record_release', 'handle_id_free', 'hash_tombstone', 'hash_untouched', 'null_descriptor',
                  'reserve', 'name_register', 'xmd_binding', 'image_list_append', 'image_list_retire'}
RESOURCE_CHECKPOINTS = {'restore-constructor-return', 'resource-retirement', 'second-title-return'}

CALL_ANCHORS = [
    # 124548 successor constructor (mode 10 branch) and its node descriptor dtor
    (0x124574, 'jal', 0x1151b8), (0x12458c, 'jal', 0x1cb4b8), (0x124598, 'jal', 0x114c80), (0x1245a0, 'jal', 0x12d470),
    (0x1245e0, 'jal', 0x1243c8), (0x1245e8, 'jal', 0x11b2f0), (0x1245fc, 'jal', 0x11f5f8), (0x12461c, 'j', 0x114c80),
    (0x124628, 'j', 0x124448),
    # 1243c8 wrapper resources: atlas through 11fb78 (no 18f500), FLD.RUW through 1a9190
    (0x1243dc, 'jal', 0x1c0930), (0x1243e8, 'jal', 0x1bccf0), (0x1243f4, 'jal', 0x11fb78), (0x1243fc, 'jal', 0x1bcc80),
    (0x124404, 'jal', 0x1c0930), (0x124410, 'jal', 0x1c0930), (0x124420, 'jal', 0x1a9190), (0x124428, 'jal', 0x1c0930),
    (0x11fb88, 'jal', 0x1c2e68), (0x11fb94, 'jal', 0x18f7a8),
    (0x11fb28, 'jal', 0x1c2e68), (0x11fb38, 'jal', 0x18f500), (0x11fb40, 'jal', 0x18f7a8),     # contrast: 11fb10
    # 18f7a8 -> 1cd1e0 / 1cd7d0 / 1cd298 (Title path, CPSM 0 branch)
    (0x18f7ec, 'jal', 0x1cd1e0), (0x18f82c, 'jal', 0x1cd7d0), (0x18f850, 'jal', 0x1cda18), (0x18f88c, 'jal', 0x1cd298),
    (0x1cd20c, 'jal', 0x1cf008), (0x1cd220, 'jal', 0x1cc880), (0x1cd274, 'jal', 0x1cba48), (0x1cd810, 'jal', 0x1cf008),
    (0x1cd9a8, 'jal', 0x1cba48),
    # wrapper cleanup 124448 and its tail into 1cca30
    (0x124458, 'jal', 0x1a9888), (0x124460, 'jal', 0x199e88), (0x124474, 'j', 0x11fbc8), (0x11fbc8, 'j', 0x1cca30),
    # wrapper per-frame callback and the cancel/success mode request
    (0x124684, 'jal', 0x114c80), (0x124698, 'j', 0x124480), (0x124498, 'jal', 0x1248f8), (0x124500, 'jal', 0x1252a8),
    (0x124514, 'jal', 0x124930), (0x12452c, 'jal', 0x124930), (0x1244cc, 'jal', 0x1248d8), (0x1244e0, 'jal', 0x124930),
    # manager node (11f5f8 -> 1152a8 desc 0x2234f0) and window construction 11f7a0
    (0x11f61c, 'jal', 0x1152a8), (0x1152cc, 'jal', 0x114e98), (0x11f6c0, 'jal', 0x11f778), (0x11f788, 'jal', 0x11f7a0),
    (0x11f7e0, 'jal', 0x198e38), (0x11f804, 'jal', 0x198650), (0x11f9d8, 'jal', 0x1a55d8), (0x120b7c, 'jal', 0x199458),
    # resource child: window pass, layout dispatch, placement RNG, glyph-cache paths, font getter
    (0x100f70, 'jal', 0x197fc0), (0x1012bc, 'jal', 0x102218), (0x101030, 'jal', 0x1980f8), (0x10104c, 'j', 0x198390),
    (0x1984ec, 'jal', 0x19d090), (0x19d0d8, 'j', 0x19d0e8), (0x19d0c8, 'j', 0x19eec8), (0x19d0d0, 'j', 0x1a1ad8),
    (0x19d530, 'jal', 0x19c568), (0x19d880, 'jal', 0x19ca20), (0x19c74c, 'jal', 0x1f14b0), (0x19cad4, 'jal', 0x1f14b0),
    (0x19cc78, 'jal', 0x1f14b0), (0x19d338, 'jal', 0x195f48), (0x19d3d4, 'jal', 0x195f48), (0x195fd0, 'jal', 0x197c68),
    (0x197eac, 'jal', 0x1976f0), (0x100244, 'jal', 0x197a98), (0x197b28, 'jal', 0x1976f0), (0x19d840, 'jal', 0x1a5990),
    (0x1a6660, 'jal', 0x1951e8), (0x1a6674, 'jal', 0x1951e8), (0x195274, 'jal', 0x1cca30), (0x198528, 'j', 0x1a8150),
    # resource-child destructor window sweep
    (0x1010a8, 'jal', 0x198550), (0x198580, 'jal', 0x19bfd8), (0x198598, 'jal', 0x199e88),
    # effect 106 reclamation (host frees) and the theme fade continuation / forced stop
    (0x1023fc, 'jal', 0x103bf8), (0x102440, 'jal', 0x103bf8), (0x12ce8c, 'jal', 0x12de98), (0x12ce98, 'jal', 0x12dcb0),
    (0x12db9c, 'jal', 0x12de98), (0x12dba8, 'jal', 0x12dcb0),
    # manager input 11e480 (Restore arms) and updater 11c2c8 (storage coroutine)
    (0x11e670, 'jal', 0x1a4e50), (0x11efb0, 'jal', 0x11faa8), (0x11efc8, 'jal', 0x1a9a48), (0x11efe4, 'jal', 0x1a9b18),
    (0x11effc, 'jal', 0x1a99e0), (0x1a9a30, 'jal', 0x198c80), (0x11f03c, 'jal', 0x1c0160), (0x11f084, 'jal', 0x1c0160),
    (0x11f074, 'jal', 0x11faf8), (0x11f0bc, 'jal', 0x11faf8), (0x11f404, 'jal', 0x12df80), (0x11f510, 'jal', 0x1248d8),
    (0x11c9c4, 'jal', 0x16d580), (0x11c9e0, 'jal', 0x16c2a0), (0x11dccc, 'jal', 0x1aa638), (0x11dd04, 'jal', 0x16d748),
    (0x11dd28, 'jal', 0x16c2a0), (0x198168, 'jal', 0x1998a8),
    # mode-2 layout 1a1ad8 placement / glyph-cache call sites
    (0x1a4258, 'jal', 0x19c568), (0x1a385c, 'jal', 0x19ca20), (0x1a47e8, 'jal', 0x19ca20), (0x1a3ed0, 'jal', 0x195f48),
    (0x1a3fdc, 'jal', 0x195f48), (0x1a401c, 'jal', 0x195ff0),
    # startup font loads (flags 0x1000 / 0x2000 / 0: none is the 0x4000 glyph-cache mode)
    (0x1001d0, 'jal', 0x1947a0), (0x1001ec, 'jal', 0x1947a0), (0x100208, 'jal', 0x1947a0),
]
WORD_ANCHORS = [
    (0x223750, 0x124548), (0x223754, 0), (0x223758, 0x124630), (0x22375c, 0x124628),     # mode 9/10 wrapper node
    (0x223760 + 4 * 7, 0x1247b0), (0x223760 + 4 * 9, 0x1247c4),                          # mode 8 / mode 10
    (0x2234f0, 0x11f668), (0x2234f4, 0x11fec8), (0x2234f8, 0x120ad0), (0x2234fc, 0x120b48),   # manager node
    (0x2233f0, 0x24f6d0), (0x2233f4, 0x19), (0x2233f8, 0x14), (0x223444, 0x1f8e98),       # Restore window descriptor
    (0x223448, 0x11b518), (0x22344c, 0x11c2c8), (0x223450, 8), (0x223454, 0x11), (0x223458, 0x10),
    (0x223460, 0),                                                                         # descriptor list terminator
    (0x1f8f48 + 4 * 8, 0x11e480),                                                          # input table entry 8
    (0x221980, 0x100ef8), (0x221984, 0), (0x221988, 0x100f90), (0x22198c, 0x101058),       # resource child
    (0x227988, 0x12c350), (0x227990, 0x12c8f8), (0x227994, 0x12c8d8),                       # mode8 node
]
STRING_ANCHORS = {0x223708: 'cmndata/2d/', 0x223718: 'main.xpf', 0x223728: 'bm_if.xim', 0x223738: 'cmndata/win/',
                  0x223748: 'fld.ruw'}
INSN_ANCHORS = [
    (0x124564, 0x8e070014, '124548 reads its parent'),
    (0x1245ac, 0x24020009, '124548 mode 9 compare'), (0x1245b4, 0x2402000a, '124548 mode 10 compare'),
    (0x124640, 0x8e03002c, '124630 phase +0x2c'), (0x124668, 0x8c42001c, '124630 reads manager node flags +0x1c'),
    (0x12466c, 0x0441000d, '124630: manager flags >= 0 (alive) -> return'),
    (0x12467c, 0xae02002c, '124630: manager dead (or absent) -> phase 1'),
    (0x1244ac, 0x2402000a, '124480 mode 10 compare'), (0x1244f4, 0x24020003, '124480: result 3 = successful load'),
    (0x1244f8, 0x5462000a, '124480: result != 3 -> title request'),
    (0x124524, 0x24050008, '124480: request mode 8'), (0x124528, 0x24060001, '124480: selector 1 (title)'),
    (0x11fb9c, 0xae22f730, '11fb78: *0x24f730 = 18f7a8 handle'),
    (0x124400, 0xae020030, '1243c8: wrapper+0x30 = atlas handle'),
    (0x124468, 0x8e040030, '124448: a0 = wrapper+0x30 for 11fbc8 -> 1cca30'),
    (0x11f618, 0xac50f734, '11f5f8: *0x24f734 = -1'),
    (0x11f9dc, 0xae220004, '11f7a0: window record +4 = *0x24f730 (atlas handle, no reference)'),
    (0x1a55f4, 0xac650008, '1a55d8: ui window +8 = atlas handle (no reference)'),
    (0x11fa50, 0x0040f809, '11f7a0: descriptor constructor jalr (11b518)'),
    (0x1023b8, 0x30620200, '102218: 0x200 -> reclaim'), (0x1023f0, 0x8ca4007c, '102218 reclaim reads element +0x7c only'),
    (0x102460, 0xac620000, '102218: slot = -1'),
    (0x195f78, 0x30620100, '195f48: font entry flag 0x100 gates the dynamic glyph cache'),
    (0x12db78, 0x1040000d, '12db50: active theme fade is stopped at once by a new stream start'),
    (0x12dd5c, 0x24020080, '12dd50: fade counter 0x80'), (0x12ce5c, 0x2442ffff, '12ce20: counter - 1'),
    (0x12ce84, 0x14400007, '12ce20: counter 0 -> 12de98, 12dcb0'),
    (0x1983e0, 0x30628000, '198390: window live flag 0x8000'), (0x198498, 0x30820020, '198390: 0x20 blocks layout'),
    (0x1984a4, 0x30820001, '198390: bit 0 = layout requested'),
    (0x19d098, 0x1062000f, '19d090: mode 1 -> 19d0e8'), (0x199410, 0x34428000, '198e38: window live flag 0x8000'),
    (0x100f70, 0x0c065ff0, '100ef8: 197fc0 clears the window table'),
    (0x100374, 0x24020020, 'cancel mask 0x20'), (0x100378, 0xac620018, '*0x1f85e0 = cancel mask'),
    (0x11f090, 0x8c6485e0, '11e480 540 arm: pressed & *0x1f85e0'), (0x11ef28, 0x240201fe, 'state 0x1fe compare'),
    (0x11ef4c, 0x2402021c, 'state 0x21c compare'), (0x11f0a8, 0x3c01c080, 'close rate -4.0'),
    (0x11f0b4, 0xaea389a8, 'state = -1 (0x1f89a8)'), (0x11e690, 0xaea289a8, 'opacity 0 -> state -2'),
    (0x11f508, 0x24040004, 'cancel result 4'), (0x11f52c, 0xae22001c, 'manager +0x1c |= 0x80000000'),
    (0x11dc7c, 0x24030212, 'state 0x212 (checking)'), (0x11dfa0, 0x2402021c, 'state 0x21c (no saved data)'),
    (0x1a9a24, 0x24020002, '1a99e0: mode 2'), (0x1a9a2c, 0xaca23bec, '1a99e0: w+0x3bec = 2'),
    (0x1a54b0, 0x34421000, '1a5488: flags |= 0x1000 (relayout every frame)'),
    (0x199904, 0x46010034, '1998a8: clamp only when opacity < min'),
    (0x194b5c, 0x34420100, '1947a0: entry flag 0x100 only on the 0x4000 path'),
    (0x1001d4, 0x24061000, 'font 0 load flag 0x1000'), (0x1001f0, 0x24062000, 'font 1 load flag 0x2000'),
    (0x10020c, 0x0000302d, 'font 2 load flag 0 (move a2, zero)'),
    (0x1a3fc0, 0x30630004, '1a1ad8: w+0x4f8 & 4 selects 195ff0'),
]

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
    for a, s in STRING_ANCHORS.items():
        if elf.cstr(a) != s:
            bad.append('string anchor %x = %r' % (a, elf.cstr(a)))
    for pc, w, why in INSN_ANCHORS:
        if elf.w32(pc) != w:
            bad.append('instruction anchor %x (%s) = %08x' % (pc, why, elf.w32(pc)))
    for pc in range(0x11fb78, 0x11fbc8, 4):
        if elf.branch_target(pc)[1] == 0x18f500:
            bad.append('11fb78 calls 18f500 at %x' % pc)
    return bad + ['title: ' + b for b in title.verify_elf(elf)] + ['retire: ' + b for b in retire.verify_elf(elf)]

class Start:
    """Handle/record FIFO, references, name hash (with tombstones) and per-handle info at the Retire stop."""

    def __init__(self, title_doc, retire_doc):
        self.model, refs, self.info, slot_of = retire.replay_title(title_doc)
        self.names = title.NameHash()
        stored = {inf['record']: title.stored_name(inf['name']) for inf in self.info.values()}
        for rec, slot in slot_of.items():
            self.names.table[slot] = rec + 8
            self.names.slot[rec] = slot
            self.names.name[rec] = stored[rec]
        self.refs = dict(refs)
        self.freed = []
        route = retire_doc['routes']['restore']
        for e in route['events']:
            k = e['kind']
            if k == 'handle_retire':
                self.model.retire(e['handle'])
            elif k == 'hash_tombstone':
                self.names.table[e['slot']] = -1
                self.names.slot[e['record']] = -1
            elif k == 'ref_adjust':
                self.refs[e['alloc']] += e['delta']
            elif k == 'free':
                del self.refs[e['alloc']]
                self.freed.append(e['alloc'])
        fs = route['final_state']
        assert self.refs == fs['references'], 'Retire final references'
        assert self.freed == fs['freed_allocations'], 'Retire freed order'
        assert self.model.in_use == fs['live_handles_in_order'] == STARTUP_HANDLES, 'Retire live handles'
        assert self.model.free_records[-75:] == fs['free_record_tail'], 'Retire free-record tail'
        assert [s for s in range(title.HASH_SIZE) if self.names.table[s] == -1] == sorted(fs['hash_tombstones'])
        assert not [s for s in range(title.HASH_SIZE) if self.names.table[s] > 0], 'Retire live hash entries'
        self.next_alloc = 1 + max(int(a[1:]) for a in list(self.refs) + self.freed)
        for h in list(self.info):
            if h not in self.model.map:
                del self.info[h]
        self._summary = json.loads(json.dumps(self._snapshot()))

    def summary(self):
        return self._summary

    def _snapshot(self):
        return {'live_allocations': len(self.refs), 'next_allocation_id': 'A%d' % self.next_alloc,
                'live_handles_in_order': list(self.model.in_use),
                'free_record_head': self.model.free_records[:8], 'free_record_count': len(self.model.free_records),
                'free_record_tail': self.model.free_records[-75:],
                'hash_tombstones': sorted(s for s in range(title.HASH_SIZE) if self.names.table[s] == -1),
                'hash_live_entries': [], 'reserved_queue_0': list(self.model.queue[0]),
                'reserved_queue_1': list(self.model.queue[1]), 'image_list': [],
                'hr_free': 'ids 36..0xfff free (one merged run after Retire retirement)'}

class Builder:
    def __init__(self, start):
        self.s = start
        self.model = start.model
        self.refs = dict(start.refs)
        self.names = start.names
        self.info = {h: dict(v) for h, v in start.info.items()}
        self.next_alloc = start.next_alloc
        self.freed = []
        self.tombstones = []
        self.events = []
        self.checkpoint = None
        self.frame = None
        self.image_list = []

    def ev(self, kind, status, pc, **kw):
        e = {'seq': len(self.events), 'checkpoint': self.checkpoint, 'frame': self.frame, 'kind': kind,
             'status': status, 'pc': pc}
        e.update(kw)
        self.events.append(e)
        return e

    def mark(self, name, frame, note):
        self.checkpoint = name
        self.frame = frame
        self.ev('checkpoint', 'proven', '-', name=name, note=note)

    def at(self, frame):
        self.frame = frame

    def alloc(self, cls, psm, w, h, pc, via, purpose):
        aid = 'A%d' % self.next_alloc
        self.next_alloc += 1
        self.refs[aid] = 0
        self.ev('alloc', 'proven', pc, id=aid, alloc_class=cls, psm=psm, width=w, height=h, via=via, purpose=purpose,
                initial_references=0, placement='replay of 0x1cf008 on the live allocator state (cursors/free runs '
                'after Retire and earlier Restore frees); not derived here [open O-ALLOC]')
        return aid

    def ref(self, aid, delta, pc, reason):
        self.refs[aid] += delta
        self.ev('ref_adjust', 'proven', pc, alloc=aid, delta=delta, references_after=self.refs[aid], reason=reason)

    def load_xim(self, b, name, reserved):
        """0x18f7a8 on owned bytes (CPSM 0 branch): 1cd1e0 (alloc, handle, upload), then per level 1cd7d0 + 1cd298."""
        x = title.parse_xim(b)
        img = self.alloc(1, x['psm'], x['width'], x['height'], '0x1cd20c', '0x18f7ec -> 0x1cd1e0', name + ' image')
        q = list(self.model.queue[0])
        h, r = self.model.create(0)
        self.ev('handle_create', 'hypothesis', '0x1cd220', handle=h, record=r, image_alloc=img, reserved_class=0,
                from_reserved_queue=bool(q), name=name,
                id_derivation='HR first-fit (0x1cfc00) or reserved queue B head; record = head of the FIFO free-record '
                              'list left by Retire/Restore retirements (Title U06 semantics)')
        self.info[h] = {'record': r, 'image_alloc': img, 'name': name, 'palettes': []}
        self.ref(img, +1, '0x1cc9dc', 'record image reference (1cc880)')
        self.ev('upload', 'proven', '0x1cd274', alloc=img, psm=x['psm'], x=0, y=0, rrw=x['width'], rrh=x['height'],
                qwc=x['qwc'], bytes=16 * x['qwc'], sha256=title.sha(x['pixels']), transfer='0x1cba48 -> 0x1c9bb8',
                source='%s +0x%x' % (name, x['pixel_offset']))
        for lvl in range(x['levels']):
            words = x['palette'][lvl * x['colors']:(lvl + 1) * x['colors']]
            data, rows = title.clut_upload_bytes(words, x['colors'])
            pal = self.alloc(0, 0, 16, rows, '0x1cd810', '0x18f82c -> 0x1cd7d0', name + ' CLUT level %d' % lvl)
            self.ev('upload', 'proven', '0x1cd9a8', alloc=pal, psm=0, x=0, y=0, rrw=16, rrh=rows, qwc=rows * 4,
                    bytes=len(data), sha256=title.sha(data), transfer='0x1cd928 -> 0x1cba48 -> 0x1c9bb8',
                    source='%s palette level %d swizzled by 0x1cd858' % (name, lvl))
            self.ev('palette_bind', 'proven', '0x18f88c', handle=h, level=lvl, alloc=pal)
            self.ref(pal, +1, '0x1cd318', 'palette level %d bind (1cd298)' % lvl)
            self.info[h]['palettes'].append(pal)
        return h, r, x

    def retire(self, h, pc_call, via):
        """0x1cca30(h) for a handle that is the only handle of its record."""
        inf = self.info[h]
        r, last = self.model.retire(h)
        E = self.ev
        E('handle_retire', 'hypothesis', pc_call, handle=h, record=r, last_handle=last, name=inf['name'], via=via,
          note='record handle count 1 -> 0 at 0x1ccaa8')
        if not last:
            raise AssertionError('Restore atlas handle is the only handle of its record')
        slot = self.names.slot.get(r, -1)
        if slot >= 0:
            self.names.table[slot] = -1
            self.names.slot[r] = -1
            self.tombstones.append(slot)
            E('hash_tombstone', 'proven', '0x1ccad4', record=r, slot=slot, value=-1)
        else:
            E('hash_untouched', 'proven', '0x1ccabc', record=r,
              note='record+0x24 == -1: 0x18f7a8 never names its handle (no 0x1cc4e0 on this path)')
        img = inf['image_alloc']
        self.refs[img] -= 1
        E('ref_adjust', 'proven', '0x1ccbd8', alloc=img, delta=-1, references_after=self.refs[img],
          reason='record image descriptor release')
        if self.refs[img] < 1:
            del self.refs[img]
            self.freed.append(img)
            E('free', 'proven', '0x1ccbe0', alloc=img, via='0x1cf728 (refcount 0)')
        for pal in inf['palettes']:
            self.refs[pal] -= 1
            E('ref_adjust', 'proven', '0x1ccbf8', alloc=pal, delta=-1, references_after=self.refs[pal],
              reason='record+8 level-0 CLUT descriptor release')
            if self.refs[pal] < 1:
                del self.refs[pal]
                self.freed.append(pal)
                E('free', 'proven', '0x1ccc00', alloc=pal, via='0x1cf728 (refcount 0)')
        E('record_release', 'hypothesis', '0x1ccc10', record=r,
          note='record node r+8 -> tail of the FIFO free-record list; fields left stale')
        E('handle_id_free', 'hypothesis', '0x1cccd0', handle=h,
          note='handle node h+8 -> free tail, map[h] = -1 (0x1ccca8), HR id freed (0x1cfca8), live count - 1')
        del self.info[h]

MANAGER = {
    'state0': '0x11e480 (state word *0x1f89a8, 0 from the ELF; 11b518 does not write it): 0x1c01d0/0x1c0330 pad + '
              'analog; 0x11e670 opacity 0x1a4e50(h) = 0.2 (not 0, because 0x1980f8 of the resource child already '
              'stepped it in S); 0x11e6a4 *0x24ecc0 = 0; 0x11e6b4 0x11b7c0 (RUW texts 7/0x10); Restore branch 0x11ef08 '
              'state 0 at 0x11ef9c: 0x11faa8(8.0) (0x198ab8 reset + tween opacity 1.0 rate 8 to 128), 0x1a9a48 stream '
              '[14,0x200b78,6,0x200b88] at w+0x26dc, 0x1a9b18(0,h,8) one command-23 reference per RUW group-8 element, '
              '0x1a99e0: w+0x3bec = 2 (0x1a9a2c, mode 2 -> 0x1a1ad8) and 0x198c80 (flags |= 1); *0x1f89a4 = 1; state '
              '0x1fe (510) at 0x11f020; 0x11b7c0 again',
    'first_update': '0x11c2c8 prefix every frame: per slot i alpha 0x24ee10[i] +-4 clamped 30..128, 0x1ab410/0x1ab600 '
                    'class RGBA of RUW elements i+10 and i, text 0x68+i; texts 4, 5 and 6 (status *0x24ed80, "" '
                    'until state 530). Coroutine line 0: 0x11bdc8, state 0x1fe (0x11e04c), 0x11bdc8, push 0x727, '
                    '0x16c370(0), 0x16d580(0x24ee78) posts the card-info job 0x16c490, poll 0x11c9e0 (busy -> line '
                    '0x396). All jalr sites of 0x11c2c8 are host heap slots *0x213574/*0x213580 on non-empty paths',
    'fade_in': 'opacity 1.0 + 8 per 0x1998a8 call (resource child, before the manager callbacks) reaches 128 (clamped, '
               'rate 0) on the 16th step; while state > 0 and opacity != *0x24ecc4 the 0x11e6c0 guard skips the '
               'switch (0x11e70c stores the new value), so Cancel cannot be consumed during the fade-in. First frame '
               'that can consume Cancel: S+17 [H]',
    'timing': 'card-thread completion times are asynchronous: 1 info job (0x16c490, up to 16 0x1d8050 retries) then '
              'one 0x16ccb8 job per slot, each posted in the call that sees the previous one complete. The number of '
              'frames in 510 and 530 is not static; observe 0x16d4f8 posts, the 0x11c9e0/0x11dd28 busy results and '
              '*0x24eea8. Cards that are absent or full restart the query with state left at 0x1fe (not this route)',
    'cancel': 'Cancel = pressed & 0x20 in state 0x1fe (0x11f03c) or 0x21c (0x11f084); no centred-stick or held-button '
              'condition; ignored in state 0x212 (no switch arm) and on fade-guard frames. Both arms: state -1, '
              '0x11faf8(-4.0) (opacity 128, rate -4), 0x12df80(4). The pending-scan variant (state 0x1fe) dies at the '
              'same C+32, but a later card completion can rewrite the state word before death [H]',
}
WINDOW = {
    'first_layout': 'S: the resource tick runs before 0x11e480, so the first layout is MODE 0 (0x19eec8) on the '
                    'constructor stream [14,0x1f9c00,6,0x1f9c2c,0] (w+0x2698 = 0x24f6d0 from the 0x198e38 third '
                    'argument): no text -> 0 glyph placements, then 16 x 0x19ca20(w,grp,i,0,0,0,-1) = 32 draws [H: G=0]. '
                    'S+1: first MODE 2 (0x1a1ad8) layout over the RUW group-8 command-23 stream. A layout runs every '
                    'frame: flags 0x1000 (0x1a5488) makes 0x198390 set bit 0x1 (0x1983ec..0x198408) and only both '
                    'opacity and rate == 0 skip it (0x1984b0..0x1984e8)',
    'death_frame': 'D = C+32: opacity 0.0 with rate still -4 -> the layout still runs (R draws); 0x11e480 then sees '
                   'opacity 0.0 and kills the manager; the window is destroyed in the same traversal (0x120b48), so no '
                   'later pass draws',
    'destroy': '0x199458 -> 0x19bfd8: 16 host frees (0x1bdaf0) of window buffers, slot 0x200688[h] = 0, *0x2006b0 - 1 '
               '(clamped at 0). No 0x1cca30/0x1cf728: the bm_if handle at window +8 is not retired, font textures '
               'are untouched',
    'ruw_free': 'host only: 0x1a9888(0) frees *0x200b08[0], *0x200ac8[0], *0x200708[0] (jalr *0x213580 at 0x1a9914) and '
                'the 0x200748[0] tables (FLD.RUW group record)',
    'slot_free': 'no-op: 0x199e88(0) returns at 0x199ed4 because *0x2005c8[0] == 0 (0x197fc0 cleared the message tables '
                 'in 0x100ef8; loader 0x199928 is unreachable on the route)',
}
RESOURCE_DTOR = ('0x101058 as in Retire with nothing to release: 0x1b5fc8/0x1891b8 skipped (+0x20 = -1, +0x34 = 0), '
                 '0x114530 (aux ids -1), 0x101240 -> 0x103640(-2) (no live effect slot after the S reclamation), '
                 '0x10d150 (count 0), 0x198550, 0x1cca30(+0x48) skipped (-1), 0x13c368, 0x1b84b0 (no instance), '
                 '0x1b6f50 (instance table empty since Retire), 0x1b5fd0 -> 0x1b2240 (image list count 0), 0x1b3fc0, '
                 '0x18fb00, 0x1b8028')
RNG_SITES = [
    {'site': '0x19c74c', 'function': '0x19c568', 'caller': '0x1a4258 (0x1a1ad8 mode 2), 0x19d530 (mode 1, unreached)',
     'condition': 'unconditional: one draw per glyph placement, including spaces, missing glyphs (code -1), '
                  'scrolled-out/hidden (-1) entries and characters cut by the 0x3be4 limit', 'status': 'proven'},
    {'site': '0x19cad4', 'function': '0x19ca20', 'caller': '0x1a385c (\\p[n] icon), 0x1a47e8 (trailing sentinel loop), '
     '0x19d880/0x19dae0 (mode 1, unreached), 0x19eec8 trailing loop (mode 0, S only)', 'condition': 'unconditional',
     'status': 'proven'},
    {'site': '0x19cc78', 'function': '0x19ca20', 'caller': 'same', 'condition': 'selector >= 32 unsigned (0x19cb2c '
     'sltiu, 0x19cbf4): every sentinel call (selector -1) and icons with n >= 32', 'status': 'proven'},
]
RNG = {
    'generator': '0x1f14b0 (libc rand; the one Session stream; value % 360 indexes the sine table 0x202ee0 and is '
                 'multiplied by 100 and 0.0, so the placement perturbation is +-0 but the draw is consumed)',
    'sites': RNG_SITES,
    'not_draw_sites': ['0x1a6908', '0x1a5990', '0x1a7b78', '0x1a8150', '0x1980f8', '0x19a150/0x1a99e0 binding',
                       'RUW setters 0x1aa2b8 0x1a9e88 0x1aa638 0x1aabb0 0x1ab410 0x1ab600 0x1aa3c0 0x1ab060',
                       '0x11b518 0x11e480 0x11c2c8 closures', '0x198e38/0x198650/0x1a51c8/0x1a5xxx setters',
                       '0x199458 0x199e88 0x1a9888 0x1a9190 0x124448', 'second 0x12c350 constructor (Title)',
                       'effect code: no live effect after the S reclamation (0x105a40/0x10c7b8 sites unreached)'],
    'mode2_pass_formula': 'R = G + sum over \\p[n] icons of (1 + [n >= 32]) + 2 * (16 - k); G = 0x19c568 calls at '
                          '0x1a4258, k = icon slot of the current group when the trailing loop 0x1a47e8 starts; '
                          '0x1a1ad8 has a single exit (0x1a48ec) and \\b still reaches the trailing loop; an empty '
                          'group costs 32',
    'per_frame': [
        {'frames': 'S', 'mode': 0, 'draws': '32 [H: no text in the constructor stream]', 'status': 'hypothesis'},
        {'frames': 'S+1..D', 'mode': 2, 'draws': 'R(frame) every frame (opacity never 0 with rate 0 until after D)',
         'status': 'proven rule, open count'},
        {'frames': 'D+1..second-title-return', 'mode': None, 'draws': 0,
         'status': 'proven (no window; no live effect; constructor has no rand)'},
    ],
    'determinants': ['number of traversals S+1..D (asynchronous card completion, Start/Cancel timing)',
                     'per-frame RUW group-8 text: status string 0x24ed80 (set at 0x11dc8c / 0x11dfc8), slot labels, '
                     'texts 4/5/6/0x68+i written every frame by 0x11c2c8 and 7/0x10 by 0x11b7c0',
                     'icon escapes \\p[n] and the current icon slot k', 'element visibility/scroll (-1 placements still '
                     'draw)'],
    'expected_total': 'not static: 32 + sum_{f=S+1..D} R(f); compute R with the native layout_mode2 over the owned '
                      'FLD.RUW group 8 and the per-frame texts, and compare with the observer counts (open O-RNG)',
}
GLYPH = {
    'verdict': 'NOT reached on this route (proven modulo indirect calls, Title U02)',
    'proof': ['startup 0x1001a8 loads font 0 IQ24 with flag 0x1000, font 1 BMUI with 0x2000, font 2 BM with 0; only '
              'load flag 0x4000 sets entry flag 0x100 (0x194b5c); the others upload a static atlas and set 0x200 '
              '(0x194b24)',
              '0x195f48 calls 0x197c68 only when entry flag 0x100 is set (0x195f78)',
              '0x195ff0 (unconditional 0x197c68) needs w+0x4f8 & 4; 0x1a56d0 sets 0x10 and no route closure stores '
              'w+0x4f8',
              'a \\f[n] font switch (0x1a3784; the RUW "\\f[2] " prefixes) cannot reach a font with 0x100',
              'main-loop 0x100228 -> 0x197a98 issues 0x1976f0 only for LRU entries with +8 >= 0; 0x1971f0 initialises '
              '+8 = -1 and only 0x197c68 (0x197e20/0x197e70) changes it, so only 0x8000 terminator tags are emitted',
              'the mode-1 path 0x19d0e8 -> 0x195f48 is not the Restore mode (w+0x3bec = 2)'],
    'glyph_cache_allocation': 'A03 (handle *0x200494, PSMT4 glyph cache from startup 0x197398): no writer on the route; '
                              'its bytes stay unknown since Startup',
    'if_reached_shape': '0x1976f0: GIFtag A+D x4 BITBLTBUF(0x50) from 0x1cc2b0(*0x200494) (DBP/DBW/DPSM), TRXPOS(0x51) '
                        'DSAX = ((slot-6)%32)*32, DSAY = ((slot-6)/32)*32, TRXREG(0x52) 16|32 x even height, '
                        'TRXDIR(0x53) 0, IMAGE GIFtag with 4bpp bytes from font entry +0x20 (stride +0x1c), zero '
                        'padded, then TEXFLUSH(0x3f)',
    'font_getter': '0x1951e8 is a 7-instruction read of entry+0x18 (0x1951e8..0x195200); callers 0x1a6660/0x1a6674 in '
                   '0x1a5990 feed 0x1cc2b0/0x1cd5a8 reads. 0x195230 (set handle; 0x1cca30 when entry flag 0x200) is '
                   'reached only via 0x1952a0 and has no site in any route closure; the Retire path '
                   '0x1a5990 > 0x1951e8 > 0x1cca30 was a function-extent artifact',
    'text_queue': '0x1958e0/0x195ad0 (0x1a8150 at 0x1a9040/0x1a9048) build the global text-list packet and its '
                  'terminator; reads only (0x1944e8 -> 0x1cc2b0/0x1cd5a8); no BITBLTBUF/TRX register ids',
}
FONTS = {
    'persistent_textures': 'font atlases uploaded at startup (Startup allocations) stay live: no 0x195230, 0x1cca30 or '
                           '0x1cf728 on any font entry in the route closures; window death 0x199458 frees host '
                           'buffers only',
    'restore_window_font': 'w+0x20c = 1 (BMUI) from 0x1a5560(h, desc+0xc = 1); \\f[n] escapes in RUW text may switch '
                           'the font for a run (0x1a3784)',
}
ATLAS = {
    'file': 'MAIN.XPF/bm_if.xim', 'psm': 19, 'cpsm': 0, 'width': 256, 'height': 320, 'levels': 1, 'colors': 256,
    'load': '0x1243f4 0x11fb78 -> 0x1c2e68 (0x11fb88) -> 0x18f7a8 (0x11fb94); no 0x18f500 (contrast 0x11fb10 at '
            '0x11fb38)',
    'owner': 'wrapper +0x30 (0x124400); also cached in *0x24f730 (0x11fb9c)',
    'borrowers': 'window record +4 (0x11f9dc) and ui window +8 (0x1a55f4): descriptor-only, no reference; read by '
                 '0x1a7b78 (0x1a7f48/0x1a7f54) and 0x1a6908 (0x1a69ec/0x1a69fc) through 0x1cc2b0/0x1cd5a8',
    'lifetime': 'loaded in S (constructor) -> borrowed by the window S..D -> window destroyed in D (no release) -> '
                'released at D+3 by the wrapper dtor 0x124448 tail 0x11fbc8 j 0x1cca30(wrapper+0x30), AFTER '
                '0x1a9888(0) and 0x199e88(0) and after the resource child dtor 0x101058; image and CLUT allocations '
                'freed there; never in the image list 0x337508',
}
ABSENCES = [
    {'what': 'allocation 0x1cf008, handle create 0x1cc880, upload 0x1cba48, reserve 0x1cd5c8, name 0x1cc4e0, '
             'retire 0x1cca30, free 0x1cf728, ref adjust 0x1cee50, XIM load 0x18f7a8, PSM override 0x18f500, image '
             'list 0x1b2348/0x1009e0, instance 0x1b8a80, job post 0x178578',
     'where': 'per-frame and teardown closures: 0x124630, 0x124480, 0x11e480, 0x11c2c8, 0x11b518, 0x11f5f8/0x11f668, '
              '0x100f90, 0x120b48, 0x199458, 0x1a9888, 0x199e88, 0x1a9190, 0x124818, 0x12ce20',
     'status': 'proven (absent-roots.txt; modulo indirect calls)',
     'exceptions': ['0x124548 -> 0x1243c8 -> 0x11fb78 -> 0x18f7a8 (the one atlas load, constructor only)',
                    '0x124448 -> 0x11fbc8 j 0x1cca30 (the one atlas retirement, wrapper dtor only)',
                    '0x101058 -> 0x1cca30 sites (0x1010bc, 0x10ce94, 0x1796f0, 0x1b227c, 0x1b60fc) all have empty '
                    'inputs on this route (Retire state)',
                    '0x100f90 > 0x114760 > 0x1afb48 > 0x1c9bb8 (aux upload) guarded by aux ids 0x24ea94/0x24ea90 = '
                    '-1; setter 0x1145c0 absent']},
    {'what': 'glyph-cache writers 0x1976f0 / 0x197c68 / 0x195ff0 and font handle setter 0x195230',
     'where': 'all route closures', 'status': 'proven not executed (guards: font entry flag 0x100 never set, '
     'w+0x4f8 & 4 never set, LRU +8 = -1); sites present only in 0x1a1ad8/0x19d0e8/0x197a98 (glyph-sites.txt)'},
    {'what': 'name-cache/lower-case/class setters 0x186a58, 0x186a48, 0x1cbdd8', 'where': 'all route closures',
     'status': 'proven (absent-roots.txt): the second title loads NIGHT_V exactly like the first'},
    {'what': 'material-table writes', 'where': 'Restore', 'status': 'proven none; the second title refills 0x3301a4 '
     'from the XMD as in Title (U09 inferred)'},
    {'what': 'GS writers besides the per-frame clear/frame list (Title W1/W2) and the bm_if constructor uploads',
     'where': 'S..D+5', 'status': 'proven modulo U02/U04: no glyph transfer, no aux upload, no filter capture'},
]
TAILS = [
    {'what': 'effect 106 job', 'status': 'proven',
     'continuation': 'reclaimed in traversal S by the Restore resource child\'s first 0x100f90 -> 0x102218 '
                     '0x1023c4..0x102460: 0x103bf8(element+0x7c) and 0x103bf8(job) host frees, slot = -1; '
                     'element+0x5c (stale handle 43) is never read; no texture operation',
     'observe': '0x1023fc/0x102440 a0, 0x102460 hit in S'},
    {'what': 'title theme stream fade', 'status': 'proven mechanism, hypothesis counts',
     'continuation': '0x12ce20 once per main-loop iteration (0x1006a0): counter 126 at S entry [Retire native/observer] '
                     '-> 0 at iteration S+125 -> 0x1757e0 volume 0, 0x12de98, *0x2d9a60 = 0, 0x12dcb0(1) (stream '
                     'close, host free). Nothing in Restore writes 0x2d9a60/64/68 (0x12d470 touches only 0x2d9a00.. '
                     'BGM state). If the route reaches D+5 before S+125 (fast cancel), the second title\'s 0x12db50 '
                     '(0x12db68) completes the fade at once instead',
     'observe': '0x12ce8c and 0x12dba0 hit frames, *0x2d9a68 at S entry'},
    {'what': 'storage jobs', 'status': 'hypothesis',
     'continuation': 'a card-info job posted in state 540 just before death stays in flight after the manager dies; '
                     'the next 0x16d4f8 caller waits for busy to clear',
     'observe': 'busy word *(*0x2e27c0+4) at D..D+5'},
    {'what': 'one-shot sounds 0x12df80(4)', 'status': 'open', 'continuation': 'completion not modeled'},
]

def window_pass(B, frame):
    B.ev('window_pass', 'proven', '0x101030', frame_label=frame,
         note='0x1980f8 (0x1994d8, 0x199808, 0x1998a8 opacity step 0 -> 0.2; per-frame pacing may set bit 0x1 at '
              '0x19833c) then 0x198390: flags 0x8000 live, 0x1000 -> bit 0x1|0x200, 0x25c = 0x5c -> 0x1a7b78 background '
              '(reads the atlas handle at window +8, 0x1a7f48), layout 0x19d090 with mode 0 in S, then j 0x1a8150 draw '
              'list (0x1958e0/0x195ad0 text-list packet)')
    rng_event(B, frame)

def rng_event(B, frame):
    if frame == 'S':
        B.ev('rng_draws', 'hypothesis', '0x19cad4/0x19cc78', frame_label='S', mode=0, count=32,
             formula='0 glyphs + 16 x 0x19ca20(w,grp,i,0,0,0,-1) x 2 (mode-0 trailing loop of 0x19eec8)')
    else:
        B.ev('rng_draws', 'open', '0x19c74c/0x19cad4/0x19cc78', frame_label=frame, mode=2, count='R(frame)',
             formula=RNG['mode2_pass_formula'], note='one mode-2 layout per traversal in this range')

def restore_construct(B, bm_if):
    E = B.ev
    B.mark('retire-successor-entry', 'S', 'Retire stop: entry of 0x124548 (a0 = wrapper node, 0x38 bytes, last child of root, '
           'descriptor 0x223750), mode 10 selected; frame S = Retire frame N+3 (same traversal continues)')
    E('state', 'proven', '0x124548', what='start state', note='Retire Restore-route final state replayed from the pinned Title '
      'and Retire contracts: 79 allocations A00..A78, handles/records 0..35, 62 hash tombstones, no live name, empty image '
      'list and reserved queues; effect-106 job reclaim-pending (flags 0x20|0x100|0x200|0x8400, stale handle 43); '
      'theme fade active (*0x2d9a60 = 1, counter *0x2d9a68 = 126 [Retire observed])')

    B.mark('restore-constructor-return', 'S', '0x124548 body through its tail call 0x12461c j 0x114c80(1.0) returning '
           'into 0x11528c')
    E('store', 'proven', '0x12456c', field='wrapper+0x28 = parent+0x44 (0), +0x24 = parent+0x4c (mode 10), +0x20 = '
      'resource child', note='0x124564 reads the parent')
    E('construct', 'proven', '0x124574', node='resource 0x221980', node_bytes=0x50, parent='wrapper', ctor='0x100ef8',
      via='0x1151b8 (0x114e98 zero-filled host block, last child, ctor jalr 0x115284)',
      note='0x100ef8: +0x3c=1, +0x40=0x40c, +0x28=0x40d, +0x20=+0x48=-1, +0x24/+0x2c/+0x30/+0x34/+0x38/+0x44/+0x4c=0; '
           '0x1cb4b8(0,0,0,0), 0x1cb538(0), 0x114cb0(0), 0x13c2d8, 0x197fc0 (window table 0x200688[0..8] = 0, '
           '*0x2006b0 = 0, 0x200520/24/28/2c = 0; no window is live here, nothing is freed)')
    E('call', 'proven', '0x12458c', callee='0x1cb4b8', args='0,0,0,0', resource='layer clear colour bytes only')
    E('call', 'proven', '0x124598', callee='0x114c80', args='0.0', resource='fade target')
    E('call', 'proven', '0x1245a0', callee='0x12d470', resource='sequence/BGM state 0x2d9a00.. (0x1709d8 stop of '
      '*0x2d9ab8 if != -1); does NOT touch the theme-stream fade words 0x2d9a60/64/68 (only writers 12ccd0, 12ce20, '
      '12db50, 12dd50)')
    E('branch', 'proven', '0x1245b8', mode=10, note='wrapper+0x24 == 10 -> 0x1245e0')
    E('call', 'proven', '0x1245e0', callee='0x1243c8', note='wrapper resources')
    E('mount', 'proven', '0x1243e8', directory='cmndata/2d/', archive='main.xpf', via='0x1c0930(0x223708), 0x1bccf0(0x223718)')
    E('file_read', 'proven', '0x11fb88', name='bm_if.xim', via='0x1243f4 0x11fb78("bm_if.xim") -> 0x1c2e68',
      member='MAIN.XPF/bm_if.xim', psm_override='none: 0x11fb78 calls 0x1c2e68 then 0x18f7a8 directly (0x11fb94); '
      'unlike 0x11fb10 (0x11fb38 0x18f500) the authored PSM19/CPSM0 is used')
    h, r, x = B.load_xim(bm_if, 'bm_if.xim', False)
    E('store', 'proven', '0x11fb9c', field='*0x24f730', value=h, meaning='last atlas handle (read by 11f7a0 at 0x11f9cc)')
    E('host_free', 'proven', '0x11fba8', what='file buffer via jalr *0x213580 (host heap)')
    E('store', 'proven', '0x124400', field='wrapper+0x30', value=h, meaning='Restore atlas handle; the wrapper owns it '
      '(released only by 0x124448 -> 0x11fbc8 j 0x1cca30); it is NOT in the image list 0x337508')
    E('unmount', 'proven', '0x1243fc', archive='main.xpf', via='0x1bcc80; 0x1c0930(0) at 0x124404')
    E('ruw_load', 'proven', '0x124420', name='fld.ruw', directory='cmndata/win/', via='0x1a9190("fld.ruw", 0)',
      resource='host RUW group record; closure of 0x1a9190 reaches only 0x1c2e68 (file read): no texture root')
    E('call', 'proven', '0x1245e8', callee='0x11b2f0', args='1', stores='*0x1f89a0 = 1 (Restore, not Save)')
    E('construct', 'proven', '0x11f61c', node='manager', node_bytes=0xef0, parent='wrapper', descriptor='0x2234f0',
      callbacks='ctor 0x11f668, cb0 0x11fec8, cb1 0x120ad0, dtor 0x120b48',
      via='0x1245fc 0x11f5f8(wrapper, 0x2233f0): *0x24f734 = -1; 0x1152a8 allocates (0x114e98), links it as the '
          'wrapper\'s LAST child (after the resource child) and calls ctor 0x11f668(node, 0x2233f0) at 0x11537c')
    E('window_create', 'proven', '0x11f7e0', via='0x11f668 -> 0x11f778 -> 0x11f7a0(w = manager+0x40, desc 0x2233f0)',
      callee='0x198e38(0x11, 0x10)', note='first free slot of 0x200688[8] (slot 0 expected [H]); host 0x6ec0-byte window '
      'and host glyph/sprite buffers (0x1bda38); mode w+0x3bec = 0; *0x2006b0 + 1; opacity 0 / rate 0.2 / max 128 '
      '(0x1a51c8); flags |= 0x8000 (0x199410). No texture/VRAM operation in the 0x11f5f8 closure')
    E('window_bind_atlas', 'proven', '0x11f9d8', handle=h, via='0x1a55d8(window, *0x24f730): ui window +8 = handle; '
      '0x11f9dc window record +4 = handle', refcount='none: a descriptor-only binding; the wrapper keeps the only '
      'reference-holding owner (handle -> record -> A-descriptors)')
    E('manager_ctor', 'proven', '0x11fa50', callee='0x11b518', note='jalr desc+0x58: command stream [14,0x1f9c00,6,'
      '0x1f9c2c] at 0x24f6d0; coroutine line 0x24ee9a = 0, depth 6; 16c3c8(0x24ee78) card block reset; status '
      '0x24ed80 = ""; three 0x1aa2b8(0, 8, 0x6b+i, "\\f[2] ") RUW text sets; no texture/glyph/RNG root')
    E('store', 'proven', '0x11fa74', field='window record +0x50 |= 0x50, +0x5c = 0x11c2c8, +0x60 = 8 (input table '
      '0x1f8f48[8] = 0x11e480)')
    E('store', 'proven', '0x124604', field='wrapper+0x34 = manager, +0x2c = 0 (phase 0)')
    E('return', 'proven', '0x12461c', callee='0x114c80', args='1.0', note='tail call; 0x124548 returns into 0x11528c')
    E('no_resource_op', 'proven', '-', scope='rest of the constructor',
      note='besides the bm_if load, no allocation/upload/handle/name/reserve/free in the 0x124548 closure '
           '(cfg.py roots); 0x18f500, 0x1009e0 (image subsystem), 0x1b2348, 0x1cd5c8, 0x1cc4e0 are not reachable')
    return h

def restore_frames(B, h):
    """Same traversal S onward through the death frame.  Frame-independent facts; counts that need observation
    are expressed as formulas with 'open' status."""
    E = B.ev
    B.mark('first-layout', 'S', 'root returns (0x1248ac +0x3c = 0, +0x38 = wrapper, 0x1008d8, phase 0); the wrapper '
           'and both children tick in the SAME traversal S; the first mode-2 layout of the Restore window is in S+1')
    E('parent', 'proven', '0x1248ac', action='root+0x3c = 0, +0x38 = wrapper; 0x1008d8 (read-only log); phase 1 -> 0')
    E('tick', 'proven', '0x115000', node='wrapper', callback='0x124630', phase=0,
      note='+0x34 = manager; manager+0x1c >= 0 (0x12466c) -> return')
    E('tick', 'proven', '0x115000', node='resource 0x221980', callback='0x100f90',
      note='+0x24 = 0: no 0x100800 image pass (no image subsystem in Restore); +0x3c = 1 -> 0x1b9be0, 0x100d40, '
           '0x1012a8, 0x114760 (aux ids -1), then 0x1980f8, 0x198390 (tail j at 0x10104c)')
    E('effect_reclaim', 'proven', '0x1023c4', slot='water job (slot 0 [Retire O03])', via='0x1012bc 0x102218',
      frees=['0x1023fc 0x103bf8(element+0x7c) host water buffer (0x4020 bytes)', '0x102440 0x103bf8(job) host job'],
      store='0x102460 slot = -1', reads='element+0x7c only (0x1023f0); element+0x5c (stale handle 43) is NOT read; '
      'no 0x1cc2b0/0x1cca30/0x1cee50 in the block', note='first ordinary effect pass after the Retire stop: the '
      'Restore resource child\'s first 0x100f90 in traversal S (Retire O08 settled statically)')
    window_pass(B, 'S')
    E('tick', 'proven', '0x114fdc', node='manager', callback='0x11fec8 -> jalr 0x11ff30 0x11e480', state_before=0,
      note=MANAGER['state0'])
    E('tick', 'proven', '0x115000', node='manager', callback='0x120ad0 -> jalr 0x120b14 0x11c2c8',
      note=MANAGER['first_update'])
    E('storage', 'proven', '0x11c9c4', job='0x16c490 card info', via='0x16d580(0x24ee78) -> 0x16d4f8 (busy flag '
      '*(*0x2e27c0+4) = 1)', poll='0x11c9e0 0x16c2a0(1, 0x24eea8)')
    E('main_loop_tail', 'proven', '0x100638', note='0x100228 -> 0x197a98 glyph transfer list; 0x12ce20 theme fade '
      'counter 126 -> 125 (0x12ce5c)')

    B.at('S+1')
    E('layout', 'proven', '0x1984ec', frame_rule=WINDOW['first_layout'])
    rng_event(B, 'S+1')

    B.mark('empty-ready', 'R', 'the 0x11c2c8 call that stores state 0x21c (540) at 0x11dfa8 after the third empty slot')
    E('fade_in', 'proven', '0x198168', note=MANAGER['fade_in'])
    E('state', 'proven', '0x11dc84', value=0x212, note='card info complete with type *0x24ee78 == 2 and free '
      '*0x24ee7c != -1 (line 0x727); status text "Checking Memory Card (PS2)..." (0x11dc8c)')
    for i in range(3):
        E('storage', 'proven', '0x11dd04', slot=i, job='0x16ccb8 open/size of "BASCUS-97129/bkmo%d.dat"' % i,
          via='0x1aa638(0,8,0x59+%d,255,255,255,keep) at 0x11dccc; 0x16d748(path, 1)' % i,
          result='0x11dd28 poll; *0x24eea8 <= 0 (no file) -> 0x11df60 next slot in the same call; ee40[%d] = 0' % i)
    E('state', 'proven', '0x11dfa8', value=0x21c, note='all ee40 == 0 -> 540; status "No saved data  for '
      '\\"Okage Shadow King\\"." (0x11dfc8); coroutine then cycles lines 0x7e4/0x7e7 re-posting card-info jobs '
      'forever (0x11c9b8/0x11dfec)')
    E('frames', 'open', '-', what='S+1..R count', note=MANAGER['timing'])
    rng_event(B, 'R')

    B.mark('cancel-death', 'D', 'C = the 0x11e480 call that consumes Cancel; D = C+32 = the death traversal')
    B.at('R+1..C')
    rng_event(B, 'R+1..C')
    B.at('C')
    E('pad', 'proven', '0x11f084', button='cancel', word='0x1c0160(0) pressed (rising edge, pad record +6)',
      mask='*0x1f85e0 = 0x20 (0x100378)', state=0x21c, note=MANAGER['cancel'])
    E('state', 'proven', '0x11f0b4', value=-1)
    E('opacity', 'proven', '0x11f0bc', via='0x11faf8(-4.0, w) -> 0x1a4d70(h, rate -4, opacity 128, min 0, max 128)')
    E('sound', 'proven', '0x11f404', callee='0x12df80', effect=4)
    B.at('C+1..C+31')
    E('fade_out', 'proven', '0x198168', note='0x1998a8: 128 - 4k for k = 1..31; layout every frame')
    rng_event(B, 'C+1..C+31')
    B.at('D')
    E('fade_out', 'proven', '0x198168', note='0x1998a8 at C+32: 128 - 32*4 = 0.0; 0.0 < min 0 is false, so the rate '
      'stays -4 (clamp only below min, 0x199904)')
    E('layout', 'proven', '0x1984ec', frame_rule=WINDOW['death_frame'])
    rng_event(B, 'D')
    E('state', 'proven', '0x11e690', value=-2, note='0x11e670 0x1a4e50(h) == 0.0')
    E('result', 'proven', '0x11f510', via='0x1248d8(4, 0, 0)', note='*0x24ecc0 == 0 on this route (only success paths '
      '0x11dbc0/0x11e374 set 1)')
    E('death', 'proven', '0x11f52c', node='manager', store='manager+0x1c |= 0x80000000', note='state reset to 0 at '
      '0x11f4e4, *0x1f89a4 = 0, *0x1f89b4 = 0')
    E('tick', 'proven', '0x115000', node='manager', callback='0x120ad0 -> 0x11c2c8', note='cb1 still runs in D '
      '(0x114ff4 tests only 0x08000000); may re-store 0x212/0x21c into 0x1f89a8 [H, observe]')
    E('destructor', 'proven', '0x11509c', node='manager', dtor='0x120b48', note='first dead pass in the same traversal')
    E('window_destroy', 'proven', '0x120b7c', via='0x199458(window handle)', note=WINDOW['destroy'])
    return E

def retirement(B, h):
    E = B.ev
    B.mark('resource-retirement', 'D+3', 'root marks the wrapper dead; descendant destructors: resource child 0x101058, '
           'then wrapper dtor 0x124628 j 0x124448')
    B.at('D+1')
    E('tick', 'proven', '0x12467c', node='wrapper', callback='0x124630', note='manager+0x1c < 0 -> phase 1 (manager node '
      'memory still linked; read before the manager\'s second dead pass)')
    E('node_free', 'proven', '0x1150cc/0x1150e8', node='manager', note='second dead pass: unlink, 0x1bdaf0 host free')
    B.at('D+2')
    E('tick', 'proven', '0x124698', node='wrapper', callback='0x124630 phase 1', note='0x124684 0x114c80(0.0); '
      '0x124480: 0x1248f8 reads (4,0,0); mode 10 and result != 3 (0x1244f8)')
    E('mode_request', 'proven', '0x12452c', via='0x124930(root, 8, 1, 0)', mode=8, selector=1,
      stores='root+0x3c = 8, +0x48 = 1, +0x50 = 0')
    E('tick', 'proven', '0x115000', node='resource 0x221980', callback='0x100f90', note='still ticks (wrapper alive); '
      'no windows (*0x2006b0 per 0x199458)')
    B.at('D+3')
    E('parent', 'proven', '0x124868', phase=0, action='request && child -> wrapper flags |= 0x80000000, root+0x38 = 0')
    E('mark_dead', 'proven', '0x115034', node='wrapper', note='first dead pass: children marked dead, recurse, '
      'then dtor: descendants first; no callbacks run')
    E('destructor', 'proven', '0x11509c', node='resource 0x221980', dtor='0x101058', note=RESOURCE_DTOR)
    E('call', 'proven', '0x1010a8', callee='0x198550', resource='windows: 0x19bfd8 on any live slot (none: the Restore '
      'window was destroyed in D) and 0x199e88(0..15) (host)')
    E('destructor', 'proven', '0x11509c', node='wrapper', dtor='0x124628 j 0x124448')
    E('call', 'proven', '0x124458', callee='0x1a9888', args='0', resource=WINDOW['ruw_free'])
    E('call', 'proven', '0x124460', callee='0x199e88', args='0', resource=WINDOW['slot_free'])
    E('atlas_retire', 'proven', '0x124474', handle=h, via='0x124468 a0 = wrapper+0x30; j 0x11fbc8 j 0x1cca30')
    B.retire(h, '0x11fbc8', 'wrapper+0x30 (bm_if atlas)')
    E('no_resource_op', 'proven', '-', scope='0x101058 for the Restore resource child', note='resource+0x20/+0x48 = -1, '
      '+0x34 = 0, image list count 0, no model instance, no XTA, no effect slot: no texture operation')

    B.mark('wrapper-free', 'D+4', 'second dead pass frees the resource child then the wrapper')
    E('parent', 'proven', '0x1248c0', phase=0, action='request && no child -> root phase 1')
    E('node_free', 'proven', '0x1150e8', node='resource 0x221980', note='recurse first: unlink, 0x1bdaf0')
    E('node_free', 'proven', '0x1150e8', node='wrapper', note='unlink, 0x1bdaf0 (only child of root)')

def second_title(B, assets):
    """D+5: root phase 1 constructs mode 8 selector 1 exactly as Title's third construction, on the live state."""
    E = B.ev
    B.mark('second-title-return', 'D+5', 'root phase 1 builds mode 8 with selector 1; STOP at the 0x12c350 return into '
           '0x11528c, before 0x12bfc8 -> 0x12c25c j 0x1296f8 (first update of the second title, excluded)')
    E('parent', 'proven', '0x124878', phase=1, action='0x1007e0(0); root+0x40 = 0x24f738 = 8, +0x44 = +0x48 (1), '
      '+0x4c = +0x50 (0)', mode=8, selector=1)
    E('construct', 'proven', '0x1247d8', mode=8, selector=1, node_descriptor='0x227988', node_bytes=0xd0, ctor='0x12c350',
      via='0x1246b0 table 0x223760[7] = 0x1247b0 -> 0x1151b8; node linked as the only child of root')
    E('store', 'proven', '0x12c388', field='mode8+0x24', value=1, meaning='selector = parent+0x44')
    E('construct', 'proven', '0x12c384', node='resource 0x221980', node_bytes=0x50, ctor='0x100ef8', parent='mode8',
      note='new resource child; 0x100ef8 -> 0x197fc0 clears the (already empty) window table again')
    E('call', 'proven', '0x12c390', callee='0x114c80', args='0.0')
    E('call', 'proven', '0x12c3a0', callee='0x100c58', args='resource, 0x40f')
    E('call', 'proven', '0x12c3b4', callee='0x1cb4b8', args='0,0,0,0')
    E('call', 'proven', '0x12c3bc', callee='0x12d470', resource='sequence/BGM stop (no texture)')
    E('mount', 'proven', '0x12c3d0', directory='cmndata/2d/', archive='scr.xpf', via='0x1c0930, 0x1bccf0')
    titles = []
    for i, (site, sa) in enumerate(title.TITLE_SITES):
        name = title.STRING_ANCHORS[sa]
        E('image_load', 'proven', hex(site), name=name, via='0x100a60 -> 0x1009e0 -> 0x1b2348', archive='scr.xpf',
          file_read='0x1b2358 0x1c2e68("%s")' % name)
        hh, r, x = B.load_xim(assets['scr_bytes'][name], name, False)
        E('host_free', 'proven', '0x1b23b0', what='file buffer via *0x213580')
        B.image_list.append(hh)
        E('image_list_append', 'proven', '0x1b23dc', index=len(B.image_list) - 1, handle=hh, list='0x337508',
          count_addr='0x337608', note='list count was reset to 0 by Retire 0x1b2294; stale image records *0x33749c are '
          'overwritten by 0x1b25a8')
        E('image_record', 'proven', '0x100a1c', slot=i, list_index=i, handle=hh, enabled=1,
          via='0x1b26a0 -> 0x1b25a8(slot, list_index|0x1000, 1)')
        titles.append(hh)
    E('store', 'proven', '0x12c4b4', field='mode8+0xcc', value=titles[-1], meaning='0x1b36c0(slot 7) = hanten.xim handle '
      '(the NEW hanten handle; the old effect element that held stale id 43 was host-freed in S)')
    E('unmount', 'proven', '0x12c50c', archive='scr.xpf', via='0x1bcc80; 0x1c0930(0) at 0x12c514')
    E('mount', 'proven', '0x12c794', directory='cmndata/2d/', archive='night_v.xpf', via='0x1c0930, 0x1bccf0')
    E('xmd_read', 'proven', '0x186c54', name='a.xmd', via='0x12c7a0 0x186c48 -> 0x1c2e90')
    E('cache_state', 'proven', '0x186b98', cache_enabled=0, lower_case_file_names=0, reserved_class=0,
      evidence='0x1fe310/0x1fe30c setters 0x186a58/0x186a48 and cls setter 0x1cbdd8 are not reachable from any Restore '
               'closure (0x124548, 0x124630, 0x11e480, 0x11c2c8, 0x11b518, 0x100f90, 0x101058, 0x124448, 0x120b48); '
               'so every NIGHT_V name is loaded again, duplicates included (no 0x1cce78 cache hit)')
    names = assets['night_v_names']
    first_res = B.model.reserve(len(names), 0)
    E('reserve', 'hypothesis', '0x186b00', reserved_class=0, count=len(names), first=first_res,
      ids=list(range(first_res, first_res + len(names))), via='0x1cd5c8: queue B released (empty), first-fit HR')
    first = -1
    for i, nm in enumerate(names):
        fname = nm + '.xim'
        E('xmd_name', 'proven', '0x186b2c', index=i, name=nm, file_name=fname, lookup=False,
          file_read='0x186bb8 0x1c2e68("%s")' % fname, member=fname.lower())
        hh, r, x = B.load_xim(assets['night_v_bytes'][fname.lower()], fname, True)
        slot, displaced = B.names.register(r, fname)
        E('name_register', 'proven', '0x186be0', handle=hh, record=r, stored=title.stored_name(fname), hash_slot=slot,
          displaced_record=displaced, via='0x1cc4e0: probe from sum(name) % 0x400, first tombstone (-1) or empty slot '
          'on the path; newest duplicate wins')
        E('host_free', 'proven', '0x186bfc', what='member buffer via *0x213580')
        if first < 0:
            first = hh
    E('xmd_names_return', 'proven', '0x186c14', first_handle=first)
    E('lock', 'proven', '0x18346c', via='0x179478(0x249890)')
    E('mesh_slots', 'hypothesis', '0x182f60', allocator='*0x330140 (0x1cfc00 range allocator, host)',
      count=assets['night_v_meshes'], note='first-fit: the slot freed by Retire 0x17970c is expected to be reused [H]')
    binding = title.xmd_binding(B.model, first, len(names))
    E('xmd_binding', 'hypothesis', '0x182ffc', first_handle=first, count=len(names), handles=binding,
      via='0x182e28: handles[i]=h; h=0x1cc778(h) over the live in-use list (creation order after Retire/Restore retirements)')
    E('host_free', 'proven', '0x186c90', what='a.xmd buffer via *0x213580')
    E('call', 'proven', '0x12c7bc', callee='0x1b8a80', args='0,0,0, mesh slot, flags 0', resource='instance table (host)')
    E('call', 'proven', '0x12c7cc', callee='0x1bb728', resource='none')
    E('unmount', 'proven', '0x12c7d4', archive='night_v.xpf', via='0x1bcc80; 0x1c0930(0)')
    E('store', 'proven', '0x12c7e4', field='mode8+0x2c,+0x30,+0xb8; 0x25990c=9999; 0x259908/10/14/18/1c=0; 0x1fb290=0',
      value=0)
    E('audio', 'proven', '0x12c86c', callee='0x12db50', args='1, "snddata/str/thema_1"',
      note='0x12db68: if the old theme fade is still active (*0x2d9a60 != 0) it is completed now: counter 0, '
           '0x1757e0 volume 0, 0x12de98, *0x2d9a60 = 0, 0x12dcb0(*0x2d9a64 = 1); otherwise 0x12ce20 already did that '
           'at counter 0 (iteration S+125 with the Retire counter 126 [H])')
    E('call', 'proven', '0x12c884', callee='0x12dea0', args='1.0, 0, 1, 0x80', resource='sound')
    E('call', 'proven', '0x12c894', callee='0x114cc0', args='1.0', resource='fade')
    E('return', 'proven', '0x12c8b4', callee='0x114c80', args='0.0', note='tail call; 0x12c350 returns to 0x11528c')
    E('stop', 'proven', '0x11528c', note='STOP: second 0x12c350(selector=1) returned; next in the same traversal is '
      '0x12bfc8 -> 0x12c25c j 0x1296f8 (EXCLUDED)')
    return {'title_handles': titles, 'binding': binding, 'first': first}

def build(start, bm_if, assets):
    B = Builder(start)
    h = restore_construct(B, bm_if)
    restore_frames(B, h)
    retirement(B, h)
    second = second_title(B, assets)
    return B, h, second

def counts_of(events):
    k = lambda kind: sum(1 for e in events if e['kind'] == kind)
    return {'events': len(events), 'allocations': k('alloc'), 'uploads': k('upload'), 'handles_created': k('handle_create'),
            'palette_binds': k('palette_bind'), 'handles_retired': k('handle_retire'), 'records_destroyed':
            k('record_release'), 'frees': k('free'), 'reservations': k('reserve'), 'names_registered': k('name_register'),
            'bindings': k('xmd_binding'), 'image_list_appends': k('image_list_append'), 'hash_tombstones':
            k('hash_tombstone'), 'host_only_effect_reclaims': k('effect_reclaim'), 'glyph_cache_writes': 0,
            'static_rng_draws': 32}

OPEN = [
    ('O01', 'Successor order in S: wrapper cb1, resource child 0x100f90 (0x1980f8 opacity 0 -> 0.2, mode-0 layout), '
     'then manager 0x11e480 (opacity 0.2 at 0x11e670) and 0x11c2c8.', 'log 0x124630, 0x100f90, 0x198168, 0x11e670 (f0 '
     'after 0x1a4e50), 0x11effc, 0x11c9c4 entries with the traversal counter at 0x100618'),
    ('O02', 'bm_if handle/record/allocations: expected handle 36, record 113, A166 image (PSM19 256x320) and A167 CLUT '
     '(16x16 PSM0); placement from the live allocator.', 'hook 0x1cd220 v0 (handle), s-reg record after 0x1cc880, '
     '0x1cf008 v0 (u64 descriptor) at 0x1cd20c/0x1cd810, 0x11fb9c v0, 0x124400 v0'),
    ('O03', 'Effect-106 reclamation in S, host frees only.', 'hook 0x1023fc/0x102440 a0 and 0x102460 store; assert no '
     '0x1cc2b0/0x1cca30 between 0x1023c4 and 0x102460'),
    ('O04', 'RNG draws per traversal from S to D, by site.', 'count 0x1f14b0 entries keyed by ra (0x19c750, 0x19cad8, '
     '0x19cc7c) per frame; log w+0x3bec, w+0x270/0x27c at 0x1984b0, a2 (k) at 0x1a47e8 and a6 (icon selector) at '
     '0x1a385c; any other ra is a contract violation'),
    ('O05', 'Card readiness timing (frames in states 0x1fe and 0x212) and the empty results.', 'log 0x16d4f8 a0 (job), '
     '0x11c9e0/0x11dd28 v0 (busy), *0x24eea8, *0x24ee78/*0x24ee7c after the info job, hit frames of 0x11dc84 and '
     '0x11dfa8'),
    ('O06', 'Cancel consumption and death frame.', 'log 0x1c0160(0) v0 at 0x11f084 (or 0x11f03c), the 0x11f0b4 hit frame '
     'C, w+0x270 each frame, 0x11e690 and 0x11f52c hit frames (expect C+32), a0 at 0x11f510 (expect 4)'),
    ('O07', 'State word after the death-frame 0x11c2c8 call (stale 0x212/0x21c re-store hazard).', 'read *0x1f89a8 at '
     '0x120ad0 return in D'),
    ('O08', 'Wrapper phase and mode request.', 'hits of 0x12467c (D+1), 0x12452c a1/a2 (D+2: 8, 1), 0x124868 (D+3)'),
    ('O09', 'Retirement order in D+3: 0x101058 before 0x124448; 0x1a9888(0), 0x199e88(0) (no-op at 0x199ed4), then '
     '0x1cca30(36) frees A166 then A167.', 'hook 0x1cca30 a0, 0x1cf728 a0 (u64), 0x199ed4 taken'),
    ('O10', 'Glyph cache absence.', 'assert no hit at 0x1976f0, 0x197c68, 0x195ff0; font entry flags *(*0x2003ec + '
     '0x3c*i) for i 0..2 have no 0x100; LRU entries *0x200484 +8 stay -1'),
    ('O11', 'Second title ids: handles 36..43 (images) and 44..110 (NIGHT_V reservation) repeat the first title; '
     'records 114..188 differ (FIFO free-record list); hash slots equal the first title\'s (tombstone reuse).',
     'hook 0x1cd220 v0, record after 0x1cc880, 0x1cd5c8 v0, 0x1cc4e0 slot write at 0x1cc700..0x1cc710, 0x182e28 '
     'handles[]'),
    ('O12', 'Second-title allocation placement after the bm_if free (allocator cursors moved by A166/A167).',
     'hook 0x1cf008 v0 for every allocation in D+5; dump the allocator at 0x11528c'),
    ('O13', 'Theme fade end: 0x12ce20 at counter 0 (iteration S+125 expected) or 0x12db50 forced completion in D+5.',
     'log *0x2d9a68 at S entry and the hit frames of 0x12ce8c / 0x12db9c'),
    ('O14', 'Mode-0 pass in S draws 32 with no glyph (constructor stream [14,6,0]).', 'w+0x2698/0x3bf0 at 0x19d0c8 in '
     'S (expect 0x24f6d0); 0x1f14b0 count in S'),
    ('O15', 'Indirect calls (Title U02): host allocator slots, 0x202ddc packet flush hook (0x1aedfc, 0x1af284, '
     '0x1af6ec, 0x1afa10), scheduler jalr, 0x175398 sound.', 'log jalr targets at those PCs once'),
]
UNRESOLVED_INHERITED = ['Title U01 allocator placement', 'Title U02 indirect calls', 'Title U04 GS register bits',
                        'Title U06 derived ids', 'Title U10 startup binding records', 'Retire O11 indirect 0x1024c8']

KINDS = {'checkpoint', 'state', 'store', 'construct', 'call', 'branch', 'mount', 'unmount', 'file_read', 'alloc',
         'handle_create', 'ref_adjust', 'upload', 'palette_bind', 'host_free', 'ruw_load', 'window_create',
         'window_bind_atlas', 'manager_ctor', 'return', 'no_resource_op', 'parent', 'tick', 'effect_reclaim',
         'window_pass', 'rng_draws', 'storage', 'main_loop_tail', 'layout', 'fade_in', 'frames', 'pad', 'opacity',
         'sound', 'fade_out', 'result', 'death', 'destructor', 'window_destroy', 'node_free', 'mode_request',
         'mark_dead', 'atlas_retire', 'handle_retire', 'hash_tombstone', 'hash_untouched', 'free', 'record_release',
         'handle_id_free', 'image_load', 'image_list_append', 'image_record', 'xmd_read', 'cache_state', 'reserve',
         'xmd_name', 'name_register', 'xmd_names_return', 'lock', 'mesh_slots', 'xmd_binding', 'audio', 'stop'}
ORDER = [('file_read', 'handle_create'), ('handle_create', 'window_bind_atlas'), ('window_create', 'window_bind_atlas'),
         ('window_bind_atlas', 'manager_ctor'), ('manager_ctor', 'effect_reclaim'), ('effect_reclaim', 'death'),
         ('pad', 'death'), ('death', 'window_destroy'), ('window_destroy', 'mode_request'),
         ('mode_request', 'mark_dead'), ('mark_dead', 'atlas_retire'), ('atlas_retire', 'handle_retire'),
         ('handle_retire', 'image_load'), ('reserve', 'xmd_binding'),
         ('xmd_binding', 'audio'), ('audio', 'stop')]

def replay(doc, title_doc, retire_doc):
    """Independent replay of the event list from the Retire final state; returns (problems, final state)."""
    bad = []

    def need(c, m):
        if not c:
            bad.append(m)
    st = Start(title_doc, retire_doc)
    model, refs, names = st.model, dict(st.refs), st.names
    info = {h: {'image_alloc': v['image_alloc'], 'palettes': list(v['palettes'])} for h, v in st.info.items()}
    nxt, freed, tomb, live_new = st.next_alloc, [], [], {}
    pending = None
    ev = doc.get('events', [])
    cps = [e.get('name') for e in ev if e.get('kind') == 'checkpoint']
    need(cps == list(CHECKPOINTS), 'checkpoint order %r' % cps)
    need(ev and ev[-1].get('kind') == 'stop' and ev[-1].get('pc') == '0x11528c', 'must end at the 0x11528c return')
    need(sum(1 for e in ev if e.get('kind') == 'stop') == 1, 'one stop')
    cp = None
    order = {}
    last_handle = None
    for i, e in enumerate(ev):
        k = e.get('kind')
        need(e.get('seq') == i, 'seq %d' % i)
        need(k in KINDS, 'kind %r' % k)
        need(e.get('status') in STATUSES, 'status at %d' % i)
        need(isinstance(e.get('pc'), str), 'pc at %d' % i)
        if k == 'checkpoint':
            cp = e.get('name')
        need(e.get('checkpoint') == cp, 'checkpoint label at %d' % i)
        if k in RESOURCE_KINDS:
            need(cp in RESOURCE_CHECKPOINTS, 'resource operation %s outside its checkpoints at %d (%s)' % (k, i, cp))
        if k in ('alloc', 'handle_create', 'upload', 'reserve', 'name_register', 'xmd_binding'):
            need(cp != 'resource-retirement', 'creation inside retirement at %d' % i)
        if k in ('handle_retire', 'free'):
            need(cp == 'resource-retirement', 'retirement outside resource-retirement at %d' % i)
        order.setdefault(k, i)
        if k == 'alloc':
            need(e.get('id') == 'A%d' % nxt, 'allocation id %s (expected A%d)' % (e.get('id'), nxt))
            refs[e.get('id')] = 0
            nxt += 1
        elif k == 'handle_create':
            q = model.queue[0][:]
            h, r = model.create(0)
            need((h, r) == (e.get('handle'), e.get('record')), 'handle_create %r -> model (%d, %d)' %
                 ((e.get('handle'), e.get('record')), h, r))
            need(e.get('from_reserved_queue') == bool(q), 'reserved-queue flag at %d' % i)
            need(e.get('image_alloc') in refs and refs.get(e.get('image_alloc')) == 0, 'handle image alloc at %d' % i)
            info[h] = {'image_alloc': e.get('image_alloc'), 'palettes': []}
            live_new[h] = r
            last_handle = h
        elif k == 'palette_bind':
            need(e.get('handle') == last_handle and e.get('alloc') in refs, 'palette bind at %d' % i)
            if e.get('handle') in info:
                info[e['handle']]['palettes'].append(e.get('alloc'))
        elif k == 'upload':
            need(e.get('alloc') in refs and e.get('alloc') not in freed, 'upload to dead allocation at %d' % i)
        elif k == 'reserve':
            s = model.reserve(e.get('count', 0), 0)
            need(s == e.get('first') and e.get('ids') == list(range(s, s + e.get('count', 0))), 'reservation ids')
        elif k == 'name_register':
            slot, displaced = names.register(e.get('record'), e.get('stored', ''))
            need((slot, displaced) == (e.get('hash_slot'), e.get('displaced_record')),
                 'name hash slot for record %s: model (%s, %s)' % (e.get('record'), slot, displaced))
        elif k == 'xmd_binding':
            need(e.get('handles') == title.xmd_binding(model, e.get('first_handle', -1), e.get('count', 0)),
                 'binding must be the live-successor walk')
        elif k == 'handle_retire':
            h = e.get('handle')
            need(h in model.map, 'retire of dead handle %s' % h)
            if h in model.map:
                rr, last = model.retire(h)
                need((rr, last) == (e.get('record'), e.get('last_handle')), 'retire record %s' % h)
                pending = (h, rr, info.get(h, {'image_alloc': None, 'palettes': []}))
        elif k in ('hash_tombstone', 'hash_untouched'):
            need(pending is not None and e.get('record') == pending[1], 'hash step record')
            s = names.slot.get(e.get('record'), -1)
            if k == 'hash_tombstone':
                need(s == e.get('slot') and s >= 0, 'tombstone slot')
                names.table[s] = -1
                names.slot[e.get('record')] = -1
                tomb.append(s)
            else:
                need(s == -1, 'record %s holds a hash slot' % e.get('record'))
        elif k == 'ref_adjust':
            a = e.get('alloc')
            need(a in refs and a not in freed, 'ref of unknown/freed %s' % a)
            if a in refs:
                refs[a] += e.get('delta', 0)
                need(refs[a] == e.get('references_after') and refs[a] >= 0, 'references_after %s' % a)
            if e.get('delta', 0) < 0:
                need(pending is not None and a in [pending[2]['image_alloc']] + pending[2]['palettes'],
                     'release of %s not owned by the retiring record' % a)
        elif k == 'free':
            a = e.get('alloc')
            need(refs.get(a, 1) == 0 and a not in freed, 'free of referenced/freed %s' % a)
            freed.append(a)
            refs.pop(a, None)
        elif k == 'record_release':
            need(pending is not None and e.get('record') == pending[1], 'record release')
        elif k == 'handle_id_free':
            need(pending is not None and e.get('handle') == pending[0], 'handle id free')
            if pending:
                live_new.pop(pending[0], None)
            pending = None
    need(pending is None, 'unfinished retirement')
    for a, b in ORDER:
        need(a in order and b in order and order[a] < order[b], 'order %s < %s' % (a, b))
    fs = {'references': dict(sorted(refs.items(), key=lambda kv: int(kv[0][1:]))), 'freed_allocations': freed,
          'live_handles_in_order': list(model.in_use), 'handle_records': [model.map[h] for h in model.in_use],
          'free_record_head': model.free_records[:8], 'free_record_tail': model.free_records[-8:],
          'hash_tombstones_added': tomb,
          'hash_live_entries': sorted([[s, r + 8] for r, s in names.slot.items() if s >= 0]),
          'reserved_queue_0': list(model.queue[0]), 'reserved_queue_1': list(model.queue[1]), 'next_allocation': 'A%d' % nxt}
    return bad, fs

def order_of(ev, kind):
    return next((i for i, e in enumerate(ev) if e.get('kind') == kind), -1)

def check(doc, title_doc=None, retire_doc=None):
    bad = []

    def need(c, m):
        if not c:
            bad.append(m)
    need(doc.get('schema') == SCHEMA, 'schema')
    need(doc.get('elf', {}).get('sha256') == ELF_SHA256, 'elf sha256')
    for path, want, key in ((TITLE_PATH, TITLE_SHA256, 'title_sha256'), (RETIRE_PATH, RETIRE_SHA256, 'retire_sha256')):
        need(doc.get('baseline', {}).get(key) == want, 'baseline %s' % key)
    if title_doc is None:
        with open(os.path.join(ROOT, TITLE_PATH), 'rb') as f:
            b = f.read()
        need(hashlib.sha256(b).hexdigest() == TITLE_SHA256, 'title contract hash')
        title_doc = json.loads(b)
    if retire_doc is None:
        with open(os.path.join(ROOT, RETIRE_PATH), 'rb') as f:
            b = f.read()
        need(hashlib.sha256(b).hexdigest() == RETIRE_SHA256, 'retire contract hash')
        retire_doc = json.loads(b)
    if bad:
        return bad
    probs, fs = replay(doc, title_doc, retire_doc)
    bad += probs
    need(doc.get('final_state') == fs, 'final state differs from the replay')
    ev = doc.get('events', [])
    # wrapper atlas: created once, retired once, never in the image list, freed before the second title
    creates = [e for e in ev if e.get('kind') == 'handle_create']
    retires = [e for e in ev if e.get('kind') == 'handle_retire']
    atlas = creates[0].get('handle') if creates else None
    need(creates and creates[0].get('name') == 'bm_if.xim' and creates[0].get('checkpoint') == 'restore-constructor-return',
         'first creation must be the wrapper atlas in the constructor')
    need([e.get('handle') for e in retires] == [atlas], 'exactly one retirement: the wrapper atlas')
    need(not [e for e in ev if e.get('kind') == 'image_list_append' and e.get('checkpoint') != 'second-title-return'],
         'the atlas is not an image-list entry (Restore has no image subsystem)')
    need([e.get('node') for e in ev if e.get('kind') == 'node_free'][-1:] == ['wrapper'] and
         order_of(ev, 'atlas_retire') < [i for i, e in enumerate(ev) if e.get('kind') == 'node_free'][-1],
         'the atlas is released before the wrapper node is freed')
    binds = [e for e in ev if e.get('kind') == 'window_bind_atlas']
    need(len(binds) == 1 and binds[0].get('handle') == atlas, 'window binds the wrapper atlas descriptor-only')
    up = [e for e in ev if e.get('kind') == 'upload' and e.get('checkpoint') == 'restore-constructor-return']
    need([(u.get('psm'), u.get('rrw'), u.get('rrh')) for u in up] == [(19, 256, 320), (0, 16, 16)],
         'bm_if uploads must be PSM19 256x320 then the 16x16 CLUT (no PSM override)')
    need([(u.get('sha256'), u.get('bytes')) for u in up] == BM_IF_UPLOADS, 'bm_if upload bytes changed')
    rc = [e for e in ev if e.get('kind') == 'effect_reclaim']
    need(len(rc) == 1 and rc[0].get('checkpoint') == 'first-layout' and rc[0].get('frame') == 'S',
         'effect-106 reclaim exactly once, in traversal S')
    dd = [e for e in ev if e.get('kind') == 'death']
    need(len(dd) == 1 and dd[0].get('pc') == '0x11f52c' and dd[0].get('frame') == 'D', 'manager death at 0x11f52c in D')
    mr = [e for e in ev if e.get('kind') == 'mode_request']
    need(len(mr) == 1 and (mr[0].get('mode'), mr[0].get('selector')) == (8, 1), 'one mode request: 8, selector 1')
    pads = [e for e in ev if e.get('kind') == 'pad']
    need(len(pads) == 1 and pads[0].get('button') == 'cancel' and pads[0].get('state') == 0x21c, 'one Cancel in 540')
    rd = [e for e in ev if e.get('kind') == 'rng_draws']
    need(rd and rd[0].get('frame') == 'S' and rd[0].get('count') == 32, 'S mode-0 pass: 32 draws')
    need([e.get('frame') for e in rd] == ['S', 'S+1', 'R', 'R+1..C', 'C+1..C+31', 'D'], 'RNG draw ranges S..D')
    need(not [e for e in ev if e.get('kind') == 'rng_draws' and e.get('checkpoint') in
              ('resource-retirement', 'wrapper-free', 'second-title-return')], 'no draws after D')
    nf = [e.get('node') for e in ev if e.get('kind') == 'node_free']
    need(nf == ['manager', 'resource 0x221980', 'wrapper'], 'node free order')
    dt = [e.get('node') for e in ev if e.get('kind') == 'destructor']
    need(dt == ['manager', 'resource 0x221980', 'wrapper'], 'destructor order')
    g = doc.get('glyph_cache', {})
    need(isinstance(g.get('verdict'), str) and g['verdict'].startswith('NOT reached'), 'glyph verdict')
    need(not [e for e in ev if e.get('kind') == 'upload' and e.get('checkpoint') in
              ('first-layout', 'empty-ready', 'cancel-death', 'wrapper-free')], 'no upload outside load checkpoints')
    rng = doc.get('rng', {})
    need([s.get('site') for s in rng.get('sites', [])] == ['0x19c74c', '0x19cad4', '0x19cc78'], 'rng sites')
    # second title
    nl = [e for e in ev if e.get('kind') == 'xmd_binding']
    need(len(nl) == 1 and nl[0].get('handles') == list(range(44, 111)), 'second-title binding 44..110')
    lists = [e.get('handle') for e in ev if e.get('kind') == 'image_list_append']
    need(lists == list(range(36, 44)), 'second-title image list 36..43')
    need(fs['live_handles_in_order'] == list(range(111)), 'live handles 0..110 at the stop')
    need(doc.get('open') and doc.get('unresolved_inherited'), 'open lists')
    c = doc.get('counts', {})
    need(c == counts_of(ev), 'counts')
    return bad

# ---------------------------------------------------------------------------

SCHEDULE = [
    {'frame': 'S', 'status': 'proven', 'what': 'Retire N+3: root phase 1 builds the wrapper (0x1247c4 -> 0x1151b8, ctor '
     '0x124548); root +0x38 = wrapper, phase 0; then in the same traversal: wrapper cb1 0x124630 (phase 0, manager alive), '
     'resource child 0x100f90 (effect-106 reclaim, window pass with a MODE-0 layout), manager cb0 0x11e480 (state 0 -> '
     '510, mode 2 bound), manager cb1 0x11c2c8 (card-info job posted); main-loop tail (0x100228, 0x12ce20 fade 125)'},
    {'frame': 'S+1..S+16', 'status': 'proven', 'what': 'fade-in to opacity 128 (rate 8); 0x11e6c0 guard skips the input '
     'switch while opacity changes; first MODE-2 layout in S+1; layouts every frame'},
    {'frame': 'S+1..R', 'status': 'open (asynchronous)', 'what': 'card info -> state 0x212 (530) -> three empty-slot '
     'jobs -> state 0x21c (540) at 0x11dfa8 in frame R'},
    {'frame': 'C', 'status': 'proven', 'what': 'first 0x11e480 call in state 0x21c with pressed & 0x20: state -1, '
     'close tween (128, rate -4), sound 4'},
    {'frame': 'D = C+32', 'status': 'proven', 'what': 'opacity 0.0 (rate -4: layout still runs); 0x11e480 state -2, '
     '0x1248d8(4,0,0), manager dead (0x11f52c); 0x11c2c8 still runs; first dead pass: 0x120b48 -> 0x199458'},
    {'frame': 'D+1', 'status': 'proven', 'what': 'wrapper 0x124630 phase 0 -> 1 (0x12467c); manager node unlinked and '
     'freed (second dead pass)'},
    {'frame': 'D+2', 'status': 'proven', 'what': 'wrapper phase 1: 0x114c80(0.0), 0x124480 -> 0x124930(root, 8, 1, 0)'},
    {'frame': 'D+3', 'status': 'proven', 'what': 'root marks the wrapper dead; dtors: resource child 0x101058, wrapper '
     '0x124448 (0x1a9888(0), 0x199e88(0), 0x1cca30(wrapper+0x30))'},
    {'frame': 'D+4', 'status': 'proven', 'what': 'root phase 1; second dead pass frees the resource child then the wrapper'},
    {'frame': 'D+5', 'status': 'proven', 'what': 'root phase 1 constructs mode 8 selector 1 (0x12c350); STOP at its '
     'return into 0x11528c'},
]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--local')
    ap.add_argument('--elf')
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
    if not a.local:
        raise SystemExit('--local is required')
    elf = title.Elf(a.elf or os.path.join(a.local, 'extracted/SCUS_971.29'))
    if elf.sha256 != ELF_SHA256:
        sys.exit('ELF hash mismatch: %s' % elf.sha256)
    bad = verify_elf(elf)
    if bad:
        sys.exit('ELF anchors failed:\n' + '\n'.join(bad))
    docs = {}
    for path, want in ((TITLE_PATH, TITLE_SHA256), (RETIRE_PATH, RETIRE_SHA256)):
        with open(os.path.join(ROOT, path), 'rb') as f:
            b = f.read()
        if hashlib.sha256(b).hexdigest() != want:
            sys.exit('%s hash changed: %s' % (path, hashlib.sha256(b).hexdigest()))
        docs[path] = json.loads(b)
    with open(os.path.join(a.local, BM_IF), 'rb') as f:
        bm_if = f.read()
    if hashlib.sha256(bm_if).hexdigest() != BM_IF_SHA256:
        sys.exit('bm_if.xim hash mismatch')
    with open(os.path.join(a.local, 'xpf-packed/CMNDATA/2D/MAIN.XPF'), 'rb') as f:
        if hashlib.sha256(f.read()).hexdigest() != MAIN_XPF_PACKED_SHA256:
            sys.exit('MAIN.XPF hash mismatch')
    assets = title.load_assets(a.local)
    title_doc, retire_doc = docs[TITLE_PATH], docs[RETIRE_PATH]
    start = Start(title_doc, retire_doc)
    B, atlas, second = build(start, bm_if, assets)
    x = title.parse_xim(bm_if)
    doc = {
        'schema': SCHEMA,
        'elf': {'path': 'local/extracted/SCUS_971.29', 'sha256': elf.sha256},
        'baseline': {'title_contract': TITLE_PATH, 'title_sha256': TITLE_SHA256, 'retire_contract': RETIRE_PATH,
                     'retire_sha256': RETIRE_SHA256, 'retire_route': 'restore', 'start_state': start.summary()},
        'boundary': {'start': 'entry of 0x124548 with mode 10 (Retire stop; jalr 0x115284 in traversal S = Retire N+3)',
                     'stop': 'return of the second 0x12c350(selector=1) into 0x11528c in traversal D+5, before '
                             '0x12bfc8 -> 0x12c25c j 0x1296f8',
                     'route': 'empty memory card (no BASCUS-97129/bkmo%d.dat in any slot), ready UI (state 0x21c), '
                              'one ordinary Cancel (pressed & 0x20), back to the title'},
        'checkpoints': list(CHECKPOINTS),
        'schedule': SCHEDULE,
        'wrapper_atlas': dict(ATLAS, handle=atlas, sha256=BM_IF_SHA256, size=len(bm_if), qwc=x['qwc'],
                              pixel_offset=x['pixel_offset'], main_xpf_packed_sha256=MAIN_XPF_PACKED_SHA256),
        'manager': MANAGER,
        'window': WINDOW,
        'fonts': FONTS,
        'glyph_cache': GLYPH,
        'rng': RNG,
        'tails': TAILS,
        'absences': ABSENCES,
        'evidence': {'dir': 'local/research/restore-source-contract-v1/', 'files': ['closure-roots.txt', 'rng-sites.txt',
                     'glyph-sites.txt', 'absent-roots.txt', 'notes-manager.txt', 'notes-window.txt', 'asm/*.txt',
                     'manifest-sha256.txt'], 'regenerate': 'sh local/research/restore-source-contract-v1/scripts/gen.sh'},
        'second_title': {'title_handles': second['title_handles'], 'binding_first': second['first'],
                         'binding_count': len(second['binding']),
                         'handle_ids': 'equal to the first title (36..43, 44..110): HR first-fit over the merged free '
                                       'run 36..0xfff once the atlas id 36 is freed; NOT a copy of the first title',
                         'records': 'differ: FIFO free-record list head after Retire/Restore (113 was used and released by '
                                    'the atlas): images 114..121, NIGHT_V 122..188',
                         'hash': 'slots re-derived by the 0x1cc4e0 probe over 62 tombstones (first tombstone or empty '
                                 'on each path); the duplicate pairs again displace the older record',
                         'allocations': 'A168..A250 (83 new ids); placement depends on the allocator cursors after '
                                        'the A166/A167 allocation and free (open O12)'},
        'events': B.events,
        'final_state': None,
        'counts': counts_of(B.events),
        'open': [{'id': i, 'question': q, 'hook': h} for i, q, h in OPEN],
        'unresolved_inherited': UNRESOLVED_INHERITED,
    }
    probs, fs = replay(doc, title_doc, retire_doc)
    doc['final_state'] = fs
    probs = check(doc, title_doc, retire_doc)
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
