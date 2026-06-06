"""Fight with ordinary pad input: choose Attack and the default target group
whenever the real battle UI (battle-live ui_callback rows) is ready for input.
No HP, RNG, formation or outcome is touched; decisions read observations only.

Success requires verified postconditions: the battle was constructed (mode 3),
the native log reports `Native victory return queued` after the battle began,
and the field is back (status scene/hero) after the ordinary result Confirms.
Budget exhaustion, a battle that never constructs, a battle that ends without
a victory line, or a field that never returns are failures.
"""
import json
import struct
from pathlib import Path

from .outcome import StageFailure, need
from .pilot import CONFIRM, UP

VICTORY = 'Native victory return queued'
FIELD_RESTORED = 'Native saved field restored scene='
CONTACT = 'Native natural encounter frame='

def last_ui(run):
    path = Path(run) / 'battle-live.jsonl'
    if not path.is_file():
        return None, None
    with path.open('rb') as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(max(0, size - 2_000_000))
        lines = f.read().decode(errors='replace').splitlines()[1:]
    ui = actors = None
    for line in reversed(lines):
        try:
            if ui is None and '"ui_callback"' in line and '"first_return"' in line:
                ui = json.loads(line)
            if actors is None and '"actors"' in line and '"callback"' in line:
                actors = json.loads(line)
        except ValueError:
            continue  # a row still being written
        if ui and actors:
            break
    return ui, actors

def hp(actors):
    out = []
    for a in (actors or {}).get('actors', []):
        b = a.get('stat_bindings') or [None]
        h = int.from_bytes(bytes.fromhex(b[0]), 'little', signed=True) if b[0] else None
        out.append((a['entity_id'], 'E' if a['enemy'] else 'P', h, b[1] if len(b) > 1 else None))
    return out

def main(c, run, budget=6000, construct_polls=100):
    """Fight the current battle. Returns evidence; raises StageFailure."""
    start = c.log_offset()
    used = 0
    presses = 0
    last_note = None
    constructed = False
    for _ in range(construct_polls):
        o = c.observe()
        if o['battle'] and o['game_mode'] == 3:
            constructed = True
            break
        c.step(4, 0)
        used += 4
    need(constructed, 'battle', 'battle never reached mode 3', last_observe_frame=o.get('observe_frame'))
    while True:
        need(used < budget, 'battle', 'battle budget exhausted', budget=budget, presses=presses)
        o = c.observe()
        if not o['battle'] or o['game_mode'] != 3:
            c.note('battle ended/changed', o['game_mode'], o['battle'], 'frame', o['observe_frame'])
            break
        ui, actors = last_ui(run)
        action = 'wait'
        if ui and ui['frame'] == o['observe_frame']:
            layer = ui['layers'][0]
            state = ui['states'][0]
            opacity = struct.unpack('<f', struct.pack('<I', layer['opacity_bits']))[0]
            ready = ui['selection_state'] <= 50
            if state == 2 and layer['argument'] == 0 and opacity >= 128 and ready:
                action = 'confirm' if layer['selection'][0] == 0 else 'up'
            elif state == 3 and layer['argument'] in (2, 5) and ready:
                action = 'confirm'
        note = (action, hp(actors))
        if note != last_note:
            c.note(o['observe_frame'], *note)
            last_note = note
        if action == 'confirm':
            c.step(1, CONFIRM)
            c.step(1, 0)
            used += 2
            presses += 1
        elif action == 'up':
            c.step(1, UP)
            c.step(1, 0)
            used += 2
        else:
            c.step(8, 0)
            used += 8
    text = c.log_since(start)
    need(VICTORY in text, 'battle', 'battle ended without a native victory return',
         end_observe=o, log_tail=text.splitlines()[-5:])
    return {'end_observe_frame': o['observe_frame'], 'ticks': used, 'confirm_presses': presses,
            'victory_line': next(l for l in text.splitlines() if VICTORY in l)}

def fight_if_needed(c, run, result_confirms=60):
    """If a contact/battle is active, fight it to a verified victory and return
    to field control. Returns None when no battle was active."""
    if c.roar_active():
        c.roar()
    start = c.log_offset()
    o = c.observe()
    if not (o['battle'] or o['game_mode'] != 2 or not o['field']):
        return None
    c.note('contact -> battle at', o['observe_frame'])
    for _ in range(200):  # wait until the battle is constructed
        o = c.observe()
        if o['battle']:
            break
        c.step(10, 0)
    evidence = main(c, run)
    s = None
    for _ in range(result_confirms):
        c.step(1, CONFIRM)
        c.step(59, 0)
        s = c.status()
        if s['scene'] is not None:
            break
    need(s is not None and s['scene'] is not None and s['hero_position'] is not None, 'battle',
         'field did not return after battle results', last_status=s)
    need(FIELD_RESTORED in c.log_since(start), 'battle', 'no native saved-field restore after the battle')
    c.note('after battle', s)
    evidence['field_after'] = s
    return evidence

__all__ = ['main', 'fight_if_needed', 'StageFailure', 'VICTORY', 'CONTACT']
