#!/usr/bin/env python3
"""Authored progression inventory: field scenes, message-bank variable writes
and field-event script references, read from owned data.

This is static discovery for route cards and the C3 coverage ledger. It does
not execute anything. Three levels are kept apart:

- FLDSET.XDB scene rows and PME message trees are parsed exactly (formats in
  src/field.cpp read_definition and src/text.cpp read_pme).
- Inline message controls (`SET(index/bank/value)`, `ADD(...)`, `CMP(...)`)
  are taken from every string under a message root, including all choice
  branches, so a listed write is reachable only if its branch is taken.
- XMT instructions are found by an aligned word scan, not by control-flow
  disassembly. Each hit is a *candidate*: it is kept only when its operands are
  valid for the opcode (a scene that exists, a message id present in the
  scene's bank, a variable bank 0..4), and it is reported with that label.

Usage:
  chapter_inventory.py --stage STAGE --decoded XPF_DECODED [--scene N ...]
                       [--variables VARIABLES_OR_SAVE] --output JSON
"""
import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

SAVE_HEADER = 12          # the native Save contract: '!crc', length 0xd78, checksum
PAYLOAD_SIZE = 0xd78
CONTROL = re.compile(rb'\b(SET|ADD|CMP)\((-?\d+)/(-?\d+)/(-?\d+)\)')

def u32(data, at):
    if at < 0 or at + 4 > len(data):
        raise ValueError(f'truncated word at {at:#x}')
    return struct.unpack_from('<I', data, at)[0]

def s32(data, at):
    return struct.unpack_from('<i', data, at)[0] if at + 4 <= len(data) else None

def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

# ---------------------------------------------------------------- FLDSET.XDB
def parse_fldset(data):
    """All scene rows of the runtime field database (src/field.cpp:179)."""
    if u32(data, 0) != 0x424458 or u32(data, 16) != 0xffffffff:
        raise ValueError('invalid field XDB header')
    count = u32(data, 12)
    base = 32 + 16 * count
    strings = base + u32(data, 4)
    if count > 65536 or strings > len(data) or strings + u32(data, 8) != len(data):
        raise ValueError('field XDB section bounds')

    def string(offset):
        at = strings + offset
        end = data.find(b'\0', at)
        if at >= len(data) or end < 0:
            raise ValueError('field XDB string bounds')
        return data[at:end].decode('latin-1')

    scenes = {}
    for i in range(count):
        row = 32 + i * 16
        name = string(u32(data, row))
        if not name.startswith('scene'):
            continue
        number = name[5:]
        if not number.isdigit() or len(number) > 6:
            raise ValueError(f'invalid field scene identifier {name!r}')
        at = base + u32(data, row + 4)
        words = u32(data, row + 8)
        if at & 3 or words < 13 or at + words * 4 > strings:
            raise ValueError('field XDB record bounds')
        scene = {'scene': int(number), 'party': u32(data, at), 'type': u32(data, at + 4),
                 'field_flags': u32(data, at + 8), 'startup': u32(data, at + 12),
                 'room_directory': string(u32(data, at + 16)), 'room': string(u32(data, at + 20)),
                 'message': string(u32(data, at + 24)), 'event': string(u32(data, at + 28)),
                 'music': s32(data, at + 40), 'secondary_music': s32(data, at + 44), 'npcs': []}
        index = 12
        while index < words and u32(data, at + index * 4):
            if index + 8 > words:
                raise ValueError('field NPC definition bounds')
            p = at + index * 4
            scene['npcs'].append({'alias': -8 - len(scene['npcs']), 'model': string(u32(data, p)),
                                  'state': s32(data, p + 4), 'talk': s32(data, p + 8),
                                  'alternate_talk': s32(data, p + 12)})
            index += 8
        if index >= words:
            raise ValueError('missing field NPC terminator')
        if scene['scene'] in scenes:
            raise ValueError(f'duplicate scene {scene["scene"]}')
        scenes[scene['scene']] = scene
    return scenes

# ----------------------------------------------------------------------- PME
def parse_pme(data):
    """Strings and (depth, string index) nodes (src/text.cpp read_pme)."""
    header = b'KOME_RXUI_DATA  '
    if len(data) < 24 or not data.startswith(header):
        raise ValueError('unsupported PME signature')
    count = u32(data, 16)
    if count > (len(data) - 24) // 8:
        raise ValueError('PME string table bounds')
    tree = 20 + count * 8
    node_count = u32(data, tree)
    if node_count > (len(data) - tree - 4) // 8:
        raise ValueError('PME node table bounds')
    at = tree + 4 + node_count * 8
    strings = []
    for i in range(count):
        index, size = u32(data, 20 + i * 8), u32(data, 24 + i * 8)
        if index != i or size == 0 or size > len(data) - at or data[at + size - 1] != 0:
            raise ValueError('invalid PME string record')
        if b'\0' in data[at:at + size - 1]:
            raise ValueError('embedded PME string terminator')
        strings.append(data[at:at + size - 1])
        at += size
    if at != len(data):
        raise ValueError('PME trailing bytes')
    nodes = []
    for i in range(node_count):
        depth, index = u32(data, tree + 4 + i * 8), u32(data, tree + 8 + i * 8)
        if index >= count or depth > node_count:
            raise ValueError('invalid PME node')
        nodes.append((depth, index))
    return strings, nodes

def leading_decimal(raw):
    m = re.match(rb'\s*([-+]?\d+)', raw)
    return int(m.group(1)) if m else None

def pme_messages(strings, nodes):
    """Message id -> every descendant string index (all branches), for numeric
    depth-0 roots after node 0, as the original numeric lookup selects them."""
    messages = {}
    current = None
    for i, (depth, index) in enumerate(nodes):
        if i == 0:
            continue
        if depth == 0:
            ident = leading_decimal(strings[index])
            current = ident
            if ident is not None:
                messages.setdefault(ident, [])
            continue
        if current is not None:
            messages[current].append(index)
    return messages

def message_controls(strings, indices):
    out = []
    for index in indices:
        for m in CONTROL.finditer(strings[index]):
            out.append({'op': m.group(1).decode(), 'index': int(m.group(2)), 'bank': int(m.group(3)),
                        'value': int(m.group(4)), 'string': index})
    return out

def bank_inventory(path):
    strings, nodes = parse_pme(Path(path).read_bytes())
    messages = pme_messages(strings, nodes)
    result = {}
    for ident, indices in messages.items():
        controls = message_controls(strings, indices)
        text = b' '.join(strings[i] for i in indices[:6])
        result[ident] = {'strings': len(indices), 'controls': controls,
                         'excerpt': re.sub(rb'\\[a-z](\[[^\]]*\])?', b'', text)[:160].decode('latin-1').strip()}
    return result

# ----------------------------------------------------------------------- XMT
def xmt_entries(data):
    """Entry N is the opcode-1 jump at 32+N*8; target = payload + word."""
    if data[:4] != b'XMT\0':
        raise ValueError('not an XMT script')
    entries = []
    n = 0
    while 32 + n * 8 + 8 <= len(data) and u32(data, 32 + n * 8) == 1:
        payload = 32 + n * 8 + 4
        entries.append((payload + u32(data, payload)) & 0xffffffff)
        n += 1
    table_end = 32 + n * 8
    if not entries or any(t < table_end or t >= len(data) or t & 3 for t in entries):
        raise ValueError('XMT entry table')
    return entries, table_end

def decode(word):
    return word & 0xff, (word >> 8) & 0xfff, struct.unpack('<i', struct.pack('<I', word))[0] >> 20

def owning_entry(entries, pc):
    """The entry with the greatest start <= pc (a code-layout hint only)."""
    best = None
    for n, start in enumerate(entries):
        if start <= pc and (best is None or start > entries[best]):
            best = n
    return best

def scan_xmt(data, scenes, message_ids):
    """Aligned candidate instructions with operand validation."""
    entries, table_end = xmt_entries(data)
    hits = []
    for pc in range(table_end, len(data) - 3, 4):
        op, a, b = decode(u32(data, pc))
        hit = None
        if op == 0x43 and b in scenes and 0 <= a < 64:
            hit = {'kind': 'scene_request', 'scene': b, 'entrance': a}
        elif op == 0x4c and a == 0 and b == 0 and pc + 8 <= len(data) and s32(data, pc + 4) in message_ids:
            hit = {'kind': 'message', 'message': s32(data, pc + 4)}
        elif op in (0x47, 0x48) and a == 0 and pc + 8 <= len(data):
            selector = u32(data, pc + 4)
            if selector >> 16 <= 4 and (selector & 0xffff) < 0x400:
                hit = {'kind': 'set' if op == 0x47 else 'add', 'bank': selector >> 16,
                       'index': selector & 0xffff, 'value': b}
        elif op == 0x0b and pc + 12 <= len(data):
            selector = u32(data, pc + 4)
            if selector >> 16 <= 4 and (selector & 0xffff) < 0x400 and 1 <= a <= 6:
                hit = {'kind': 'if', 'bank': selector >> 16, 'index': selector & 0xffff,
                       'mode': ['', '==', '!=', '>', '<', '>=', '<='][a], 'value': s32(data, pc + 8),
                       'false_target': pc + 4 + (b >> 2) * 4 - 4}
        elif op == 0x62 and pc + 8 <= len(data):
            hit = {'kind': 'chapter_request', 'a': a, 'value': s32(data, pc + 4)}
        if hit:
            hit.update(pc=pc, entry_hint=owning_entry(entries, pc), label='candidate')
            hits.append(hit)
    return {'entries': len(entries), 'entry_offsets': entries, 'candidates': hits}

# ------------------------------------------------------------- saved state
def variables_from(path):
    raw = Path(path).read_bytes()
    if raw[:4] == b'!crc':
        payload = raw[SAVE_HEADER:SAVE_HEADER + PAYLOAD_SIZE]
        if len(payload) != PAYLOAD_SIZE:
            raise ValueError('truncated save payload')
        return payload
    return raw

def progress_state(variables):
    """The progression words named in docs (story byte, bank-4 words)."""
    def b4(i):
        return struct.unpack_from('<i', variables, 0x510 + 4 * i)[0]
    story = struct.unpack_from('<b', variables, 0x210)[0]
    return {'story': story, 'scene': b4(0), 'entrance': b4(1), 'money': b4(3), 'b4_6': b4(6),
            'objective': b4(10), 'followers': b4(13)}

def bit_flag(variables, bank, index):
    """MSB-first bit banks 0 (0x10) and 1 (0x110), src/event.cpp."""
    base = 0x10 if bank == 0 else 0x110
    byte = variables[base + index // 8]
    return (byte >> (7 - index % 8)) & 1

# --------------------------------------------------------------------- main
def build(stage, decoded, scene_filter=None, variables=None):
    fldset_path = Path(decoded) / 'FLDDATA/FLDSET.XDB'
    scenes = parse_fldset(fldset_path.read_bytes())
    banks = {}
    report = {'schema': 'okage.chapter-inventory.v1', 'scope': __doc__.strip().splitlines()[0],
              'inputs': {'fldset': {'path': str(fldset_path), 'sha256': sha256(fldset_path)}},
              'scenes': {}, 'banks': {}}
    selected = sorted(scenes) if not scene_filter else sorted(scene_filter)
    for n in selected:
        scene = scenes[n]
        bank_name = scene['message'].upper()
        if bank_name and bank_name not in banks:
            p = Path(stage) / 'FLDDATA/MES' / f'{bank_name}.PME'
            if p.exists():
                banks[bank_name] = bank_inventory(p)
                report['inputs'][p.name] = {'path': str(p), 'sha256': sha256(p)}
        ids = set(banks.get(bank_name, {}))
        entry = dict(scene)
        script = Path(decoded) / 'FLDDATA/EVENT/EVDATA.XPF' / f'{scene["event"].lower()}.xmt'
        if scene['event'] and script.exists():
            entry['script'] = {'path': str(script), 'sha256': sha256(script), **scan_xmt(script.read_bytes(), scenes, ids)}
            entry['script'].pop('entry_offsets')
        else:
            entry['script'] = None
        report['scenes'][n] = entry
    for name, messages in banks.items():
        report['banks'][name] = {str(k): v for k, v in messages.items() if v['controls']}
    if variables:
        v = variables_from(variables)
        report['state'] = {'path': str(variables), 'sha256': sha256(variables), **progress_state(v)}
    return report

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--stage', required=True)
    ap.add_argument('--decoded', required=True)
    ap.add_argument('--scene', type=int, action='append')
    ap.add_argument('--variables')
    ap.add_argument('--output', required=True)
    a = ap.parse_args(argv)
    out = Path(a.output)
    if out.exists():
        print(f'refusing existing output {out}', file=sys.stderr)
        return 2
    report = build(a.stage, a.decoded, a.scene, a.variables)
    out.write_text(json.dumps(report, indent=1) + '\n')
    print(f'{len(report["scenes"])} scenes, {sum(len(b) for b in report["banks"].values())} messages with controls')
    return 0

if __name__ == '__main__':
    sys.exit(main())
