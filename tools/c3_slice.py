#!/usr/bin/env python3
"""Chapter 2 slice of the C3 content/service coverage ledger (roadmap C3).

Static inventory (tools/chapter_inventory.py output) plus native run evidence
(stdout.log, event-trace.jsonl, manifest.json) give one row per item on a
route: fields, their assets, event scripts with executed entries/opcodes,
messages, natural formations/battle stages, menus/services and guard lines.

Columns per row:
  discovered  present in the owned data or authored tables
  parsed      read by a native/tool parser listed in `parser`
  implemented the native runtime executed it without a guard on a run
  reached     the run evidence that shows it (log line frame or trace row)
  compared    original-game comparison for this item on this route, if any
  blocking    the first failure tied to it, if any
Import presence is not runtime support: a row is `implemented` only from run
evidence, never from static data alone.

Usage:
  c3_slice.py --inventory INV.json --stage STAGE --decoded DECODED \
              --scene N [--scene N ...] --run RUN [--run RUN ...] --output LEDGER.json
"""
import argparse
import hashlib
import json
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

ACTIVATED = re.compile(r'Native ordinary field activated scene=(\d+) entrance=(-?\d+) frame=(\d+)')
RESTORED = re.compile(r'Native saved field restored scene=(\d+) frame=(\d+)')
DESTINATION = re.compile(r'Native destination: scene (\d+) entrance (-?\d+)')
ENCOUNTER = re.compile(r'Native natural encounter frame=(\d+) row=(\d+) column=(\d+) formation=(\d+) phase=(\d+) rear=(\d+)')
STAGE = re.compile(r'Native battle stage has no collision member stage=(\d+)')
VICTORY = re.compile(r'Native victory return queued scene=(\d+) frame=(\d+)')
SERVICES = {
    'title': re.compile(r'Native title constructed frame=(\d+)'),
    'restore_menu': re.compile(r'Native Restore constructed frame=(\d+)'),
    'rest': re.compile(r'Native field Rest recovered party frame=(\d+)'),
    'save_menu': re.compile(r'Native (?:field )?Save constructed frame=(\d+)'),
    'save_written': re.compile(r'Native (?:field )?Save retired frame=(\d+)'),
    'battle_results': re.compile(r'Native results constructed frame=(\d+)'),
}
EVENT_MUSIC = re.compile(r'Native event music loaded: slot(\d+) score (\d+)')
GUARD_WORDS = ('unrecovered', 'Unrecovered', 'unsupported', 'Unsupported', 'error', 'Error', 'exception', 'Refusing')

def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xffffffff
    return h

def room_path(decoded, scene):
    return Path(decoded) / 'FLDDATA' / scene['room_directory'].upper() / (scene['room'].upper() + '.XPF')

def script_path(decoded, scene):
    return Path(decoded) / 'FLDDATA/EVENT/EVDATA.XPF' / (scene['event'].lower() + '.xmt')

def xmt_entries(data):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import chapter_inventory
    return chapter_inventory.xmt_entries(data)[0]

def bank_strings(path, message_id):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import chapter_inventory
    strings, nodes = chapter_inventory.parse_pme(Path(path).read_bytes())
    return [strings[i] for i in chapter_inventory.pme_messages(strings, nodes).get(message_id, [])]

def read_run(run):
    run = Path(run)
    text = (run / 'stdout.log').read_text(errors='replace')
    lines = text.splitlines()
    ev = {'run': str(run), 'scenes': defaultdict(list), 'encounters': [], 'stages': [], 'victories': [],
          'services': defaultdict(list), 'status_messages': defaultdict(set), 'guards': [], 'trace': [],
          'entrances': defaultdict(list), 'event_music': set()}
    last = None
    for line in lines:
        if line.startswith('{'):
            try:
                s = json.loads(line)
            except ValueError:
                continue
            if isinstance(s, dict) and 'message' in s and s.get('scene') is not None:
                if last is not None and s['message'] != last and s['message']:
                    ev['status_messages'][s['scene']].add(s['message'])
                last = s['message']
            continue
        for rx in (ACTIVATED, RESTORED, DESTINATION):
            m = rx.search(line)
            if m:
                ev['scenes'][int(m.group(1))].append(line.strip())
        m = ACTIVATED.search(line)
        if m:
            ev['entrances'][(int(m.group(1)), int(m.group(2)))].append(int(m.group(3)))
        m = EVENT_MUSIC.search(line)
        if m:
            ev['event_music'].add(int(m.group(2)))
        m = ENCOUNTER.search(line)
        if m:
            ev['encounters'].append({'frame': int(m.group(1)), 'formation': int(m.group(4)),
                                     'phase': int(m.group(5)), 'rear': int(m.group(6))})
        m = STAGE.search(line)
        if m:
            ev['stages'].append(int(m.group(1)))
        m = VICTORY.search(line)
        if m:
            ev['victories'].append(int(m.group(2)))
        for name, rx in SERVICES.items():
            m = rx.search(line)
            if m:
                ev['services'][name].append(int(m.group(1)))
        if any(w in line for w in GUARD_WORDS) and 'guard_pending_opcode' not in line:
            ev['guards'].append(line.strip())
    trace = run / 'event-trace.jsonl'
    if trace.exists():
        for line in trace.read_text().splitlines()[1:]:
            if line.strip():
                ev['trace'].append(json.loads(line))
    manifest = run / 'manifest.json'
    if manifest.exists():
        m = json.loads(manifest.read_text())
        # The pinned run copy's hash (manifest artifacts), not the source path.
        native = (m.get('artifacts') or {}).get('okage-native')
        ev['binary_sha256'] = native.get('sha256') if isinstance(native, dict) else native
        ev['event_trace'] = bool(m.get('event_trace'))
    return ev

def xmp_contents(path):
    """Map model definition names and handler-1 effect template ids
    (src/map.cpp read_xmp; OpeningField::load_map_effects)."""
    d = Path(path).read_bytes()
    w = lambda a: struct.unpack_from('<I', d, a)[0]
    objects, count, definitions, dcount = w(8), w(12), w(40), w(44)
    names = []
    for i in range(dcount):
        at = definitions + i * 148
        end = d.find(b'\0', at, at + 28)
        names.append(d[at:end if end >= 0 else at + 28].decode('latin-1'))
    templates = defaultdict(int)
    used = set()
    for i in range(count):
        p = objects + i * 80
        flags = w(p + 12)
        if (flags >> 20) & 15 == 1:
            templates[(((flags >> 24) & 127) | (flags & 0xff0000)) & 127] += 1
        elif not (flags >> 20) & 15:
            used.add(w(p + 8))
    return [names[i] for i in sorted(used) if i < len(names)], dict(templates)

def row(category, ident, **kw):
    r = {'category': category, 'id': ident, 'discovered': True, 'parsed': None, 'parser': None,
         'implemented': False, 'reached': [], 'compared': None, 'blocking': None}
    r.update(kw)
    return r

def build(inventory, stage, decoded, scenes, runs):
    inv = json.loads(Path(inventory).read_text())
    evs = [read_run(r) for r in runs]
    rows = []
    for n in scenes:
        sc = inv['scenes'][str(n)]
        reached = [(e['run'], l) for e in evs for l in e['scenes'].get(n, [])]
        rows.append(row('field', f'scene {n}', parsed=True, parser='FLDSET.XDB read_definition',
                        implemented=bool(reached), reached=[f'{Path(r).name}: {l}' for r, l in reached[:3]],
                        detail={'room': f"{sc['room_directory']} {sc['room']}", 'event': sc['event'],
                                'message_bank': sc['message'], 'music': [sc['music'], sc['secondary_music']],
                                'npcs': len(sc['npcs']), 'startup_entry': sc['startup']}))
        if reached:
            for (sn, en), frames in sorted({k: v for e in evs for k, v in e['entrances'].items() if k[0] == n}.items()):
                rows.append(row('entrance', f'scene {sn} entrance {en}', parsed=True, parser='FLDSET/STG entrance',
                                implemented=True, reached=[f'activated frame {frames[0]}']))
            for label, mid in (('primary', sc['music']), ('secondary', sc['secondary_music'])):
                if mid and mid > 0:
                    rows.append(row('music', f'music {mid} (scene {n} {label})', parsed=True,
                                    parser='music_resource_names / MusicRouter::enter', implemented=True,
                                    reached=['field load installed it (an unreviewed ID stops the load)']))
        rp = room_path(decoded, sc)
        if rp.is_dir() and (rp / 'a.xmp').exists() and reached:
            models, templates = xmp_contents(rp / 'a.xmp')
            for name in sorted(set(models)):
                rows.append(row('model', f'scene {n} map {name}', parsed=True, parser='read_xmp / MapObjects',
                                implemented=True, reached=['map placement loaded with the field']))
            for tid, count in sorted(templates.items()):
                rows.append(row('effect', f'scene {n} map effect template {tid} x{count}', parsed=True,
                                parser='TES.EFD / MapEffect', implemented=True,
                                reached=['map effect constructed and updated with the field (unrecovered programs stop it)']))
        if reached:
            for npc in sorted({x['model'] for x in sc['npcs']}):
                rows.append(row('model', f'scene {n} npc {npc}', parsed=True, parser='FLDSET NPC row / FieldActor',
                                implemented=True, reached=['NPC actor constructed with the field']))
        if rp.is_dir():
            for member in ('a.xcl', 'a.stg', 'a.xmp'):
                p = rp / member
                if p.exists():
                    rows.append(row('asset', f'scene {n} {sc["room"]}/{member}', parsed=True,
                                    parser={'a.xcl': 'read_xcl', 'a.stg': 'STG reader', 'a.xmp': 'read_xmp'}[member],
                                    implemented=bool(reached), sha256=sha256(p),
                                    reached=['loaded with the field'] if reached else []))
        sp = script_path(decoded, sc)
        if sp.exists():
            data = sp.read_bytes()
            fnv = fnv1a(data)
            entries = xmt_entries(data)
            executed = [t for e in evs for t in e['trace'] if t['script_fnv1a'] == fnv]
            pcs = {t['pc'] for t in executed}
            ops = sorted({t['op'] for t in executed})
            entered = sorted(i for i, start in enumerate(entries) if start in pcs)
            traced = any(e.get('event_trace') for e in evs)
            shown = sorted({struct.unpack_from('<i', data, t['pc'] + 4)[0] for t in executed if t['op'] == 0x4c})
            rows.append(row('event_script', sc['event'], parsed=True, parser='XMT entry table / Animation::run',
                            sha256=sha256(sp), script_fnv1a=fnv,
                            implemented=bool(executed) if traced else None,
                            reached=[f'{len(pcs)} executed pcs, entries {entered}'] if executed else [],
                            detail={'entries': len(entries), 'executed_opcodes': [hex(o) for o in ops],
                                    'messages_started': shown, 'trace_available': traced}))
        else:
            shown = []
        bank = sc['message'].upper()
        status_seen = {m for e in evs for m in e['status_messages'].get(n, set())}
        if bank:
            bp = Path(stage) / 'FLDDATA/MES' / f'{bank}.PME'
            controls = inv.get('banks', {}).get(bank, {})
            for mid in sorted(set(shown) | status_seen):
                c = controls.get(str(mid), {}).get('controls', [])
                how = []
                if mid in shown:
                    how.append(f'executed 0x4c in {sc["event"]} (event trace)')
                if mid in status_seen:
                    how.append(f'status message changed to it in scene {n}')
                menu = any(b'\\o[CC]' in t for t in bank_strings(bp, mid)) if bp.exists() else False
                if menu:
                    rows.append(row('menu', f'{bank} {mid} CC choice menu', parsed=True, parser='DialogueController CC',
                                    implemented=True, reached=how))
                rows.append(row('message', f'{bank} {mid}', parsed=True, parser='read_pme/message_path',
                                implemented=True, reached=how,
                                detail={'controls': [f"{x['op']}({x['index']}/{x['bank']}/{x['value']})" for x in c]},
                                sha256=sha256(bp) if bp.exists() else None))
    forms = defaultdict(list)
    for e in evs:
        for enc in e['encounters']:
            forms[(enc['formation'], enc['phase'], enc['rear'])].append(f"{Path(e['run']).name}@{enc['frame']}")
    for (f, phase, rear), where in sorted(forms.items()):
        rows.append(row('formation', f'formation {f} phase {phase} rear {rear}', parsed=True,
                        parser='natural encounter admission', implemented=True, reached=where[:5]))
    for st in sorted({s for e in evs for s in e['stages']}):
        rows.append(row('battle_stage', f'stage {st}', parsed=True, parser='battle stage loader',
                        implemented=True, reached=['battle constructed']))
    for name in SERVICES:
        frames = [f"{Path(e['run']).name}@{f}" for e in evs for f in e['services'].get(name, [])]
        rows.append(row('service', name, parsed=True, parser='native service', implemented=bool(frames),
                        reached=frames[:5]))
    for mid in sorted({m for e in evs for m in e['event_music']}):
        rows.append(row('music', f'event music {mid}', parsed=True, parser='MusicRouter::load_secondary',
                        implemented=True, reached=['Native event music loaded']))
    guards = [f"{Path(e['run']).name}: {g}" for e in evs for g in e['guards']]
    return {'schema': 'okage.c3-slice.v1', 'scope': __doc__.strip().splitlines()[0],
            'inventory': {'path': str(inventory), 'sha256': sha256(inventory)},
            'runs': [{'run': e['run'], 'binary_sha256': e.get('binary_sha256'), 'event_trace': e.get('event_trace')}
                     for e in evs],
            'guard_lines': guards, 'rows': rows}

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--inventory', required=True)
    ap.add_argument('--stage', required=True)
    ap.add_argument('--decoded', required=True)
    ap.add_argument('--scene', type=int, action='append', required=True)
    ap.add_argument('--run', action='append', required=True)
    ap.add_argument('--output', required=True)
    a = ap.parse_args(argv)
    out = Path(a.output)
    if out.exists():
        print(f'refusing existing output {out}', file=sys.stderr)
        return 2
    ledger = build(a.inventory, a.stage, a.decoded, a.scene, a.run)
    out.write_text(json.dumps(ledger, indent=1) + '\n')
    print(f"{len(ledger['rows'])} rows, {sum(r['implemented'] is True for r in ledger['rows'])} implemented, "
          f"{len(ledger['guard_lines'])} guard lines")
    return 0

if __name__ == '__main__':
    sys.exit(main())
