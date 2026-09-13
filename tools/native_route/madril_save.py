"""Route card persistence step: from field control in Madril (scene 14) after
the arrival endpoint, walk to the Madril Overlook Hotel (event 17 Confirm pad,
scene 107 entrance 0), talk to the innkeeper (e_hotel1 message 30125: rows
"Take a break" / "Change party members" / "Save"), take Save, keep the default
slot and confirm the overwrite. The procedure is the Sonnet operator's
rehearsal on c2p-madril-v1i (explore-journal.jsonl, views h01..h16), ordinary
input only.

The Save is verified by home_inside.verify_save (target changed and well
formed, other saves unchanged, exactly one Save constructed/retired pair,
process alive, field control in scene 107 and ordinary movement), plus the
saved payload's boundary state: objective 11, b1[125] 1, story 7, scene word
107 (e_hotel1 sets the entrance word to 0 before the Save request).
"""
from pathlib import Path

from . import battle, home_inside, madril, savefmt
from .outcome import need
from .pilot import CONFIRM, DOWN, LEFT, RIGHT, UP, in_field_control

HOTEL = 107
MADRIL = 14
DOOR_PATH = [(-20.9, -127.1), (-56.9, -54.5), (-39.4, -57.2), (-32.4, -48.7), (-46.1, -35.4), (-58.3, -39.1)]
DOOR = (-61.7, -50.0)
SAVE_STATE = {'story': 7, 'scene_word': HOTEL, 'objective': 11, 'b1_125': 1}

def wait_scene(c, run, scene, polls=400):
    for _ in range(polls):
        s = c.status()
        if s['scene'] == scene and not s['event_running'] and s['hero_position'] is not None:
            return s
        if c.observe()['battle']:
            battle.fight_if_needed(c, run)
            continue
        c.step(10, 0)
    return c.status()

def to_hotel(c, run):
    misses = 0
    for x, z in DOOR_PATH:
        s = c.walk_to(x, z, tol=3, max_ticks=500, stop=lambda s: s['event_running'] or s['scene'] != MADRIL)
        battle.fight_if_needed(c, run)
        s = c.status()
        need(s['scene'] == MADRIL, 'hotel', 'left Madril before the hotel door', status=s)
        misses = 0 if madril.navigate.near(s, (x, z), 4) else misses + 1
        need(misses < 3, 'hotel', 'walk to the hotel stalled', status=s, target=(x, z))
    s = c.walk_to(*DOOR, tol=1.0, max_ticks=500)
    need(madril.navigate.near(s, DOOR, 2), 'hotel', 'hotel door pad not reached', status=s)
    c.step(2, 0)
    c.step(1, CONFIRM)
    c.step(30, 0)
    s = wait_scene(c, run, HOTEL)
    need(in_field_control(s, HOTEL), 'hotel', 'hotel (scene 107) not entered with field control', status=s)
    return s

def save_inputs(c, tag):
    """Rehearsed hotel menu inputs. Returns (after, moved) like home_inside."""
    c.step(20, UP)              # to the counter
    c.step(1, 0)
    c.step(1, CONFIRM)
    c.step(40, 0)
    o = c.observe()
    for _ in range(12):
        if o['message'] == 30125 and o['text_progress'] >= 1:
            break
        c.step(1, CONFIRM)
        c.step(40, 0)
        o = c.observe()
    need(o['message'] == 30125 and o['text_progress'] >= 1, 'save', 'innkeeper menu page not reached', observe=o)
    c.step(20, 0)
    c.send(f'view {tag}-menu')
    # First Down arms the CC menu without moving; later Downs move one row.
    for _ in range(5):
        if o['choice'] == 2:
            break
        c.step(1, DOWN)
        c.step(20, 0)
        o = c.observe()
    need(o['choice'] == 2, 'save', 'hotel menu cursor not on Save (row 2)', observe=o)
    start = c.log_offset()
    c.step(1, CONFIRM)
    c.step(40, 0)
    # Turn the text until the Save screen exists (its construction log line);
    # never press ahead of it.
    for _ in range(8):
        if 'Native field Save constructed' in c.log_since(start):
            break
        c.step(1, CONFIRM)
        c.step(40, 0)
    need('Native field Save constructed' in c.log_since(start), 'save', 'hotel Save screen never constructed')
    c.step(60, 0)
    c.send(f'view {tag}-slots')
    c.step(1, CONFIRM)
    c.step(60, 0)
    for _ in range(2):          # first Left arms, second selects Yes
        c.step(1, LEFT)
        c.step(20, 0)
    c.send(f'view {tag}-replace-yes')
    c.step(1, CONFIRM)          # "Saving completed."
    c.step(120, 0)
    for _ in range(6):
        if 'Native field Save retired' in c.log_since(start):
            break
        c.step(1, CONFIRM)
        c.step(60, 0)
    c.step(400, 0)
    after = c.status()
    c.send(f'capture {tag}-after-save', f'view {tag}-after-save-view')
    c.step(20, RIGHT)
    c.step(1, 0)
    moved = c.status()
    return after, moved

def main(c, run, manifest, target, record, tag='hotel'):
    record('hotel', {'entered': to_hotel(c, run)})
    before = home_inside.saves_snapshot(run)
    start = c.log_offset()
    after, moved = save_inputs(c, tag)
    evidence, reasons = home_inside.verify_save(run, manifest, target, before, c.log_since(start), after, moved,
                                                c.alive(), scene=HOTEL)
    need(not reasons, 'save', 'hotel Save postconditions failed', reasons=reasons, evidence=evidence)
    data = (Path(run) / 'saves' / target).read_bytes()
    state = madril.progress(savefmt.payload(data))
    need(state == SAVE_STATE, 'save', 'saved payload is not the boundary state', state=state)
    evidence['saved_state'] = state
    record('save', evidence)
    return evidence
