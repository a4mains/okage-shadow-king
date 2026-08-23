"""Route card the Madril route card: cold Restore of the pinned
Chapter 2 save, out of the house and through the gate, the planned road walk
(madril_plan.WAYPOINTS, ordinary input, natural encounters fought), Confirm on
the scene-1 event-32 trigger, and the Madril (scene 14) arrival.

Pass requires every fixed endpoint condition of the card:
  arrival  scene 14 entered at entrance 1 by the field's own transition after
           the Confirm on the trigger (log: queued scene=14 entrance=1);
  message  message 21020 observed in status and its event finished with
           ordinary Confirms;
  state    a variables capture after the event: objective (bank-4 word 10,
           +0x538) == 11, b1[125] == 1, story byte (+0x210) == 7, bank-4
           word 0 == 14;
  control  event not running, the hero moves under an ordinary pad step in
           scene 14, no guard/error line in the native log since Restore.
"""
import struct
from pathlib import Path

from . import battle, home, home_inside, madril_plan, navigate, restore_check, restore_route, savefmt
from .outcome import need
from .pilot import CONFIRM, RIGHT, UP, hero_moved, in_field_control

SCENE = 14
ENTRANCE = 1
MESSAGE = 21020
OBJECTIVE_AFTER = 11
STORY = 7
TRANSITION = f'queued scene={SCENE} entrance={ENTRANCE}'
GUARD_WORDS = ('unrecovered', 'Unrecovered', 'guard', 'error', 'Error', 'exception', 'Refusing')

def progress(variables):
    """Story byte, bank-4 words 0/10, bit flag b1[125] (src/event.cpp banks)."""
    def b4(i):
        return struct.unpack_from('<i', variables, 0x510 + 4 * i)[0]
    flag = (variables[0x110 + 125 // 8] >> (7 - 125 % 8)) & 1
    return {'story': struct.unpack_from('<b', variables, 0x210)[0], 'scene_word': b4(0),
            'objective': b4(10), 'b1_125': flag}

def guard_lines(text):
    return [l for l in text.splitlines()
            if any(w in l for w in GUARD_WORDS) and not l.startswith('{') and 'guard_pending_opcode' not in l]

def walk_road(c, run, record, max_misses=3, max_events=80):
    """home.walk_points with a stall stop: after `max_misses` consecutive
    waypoints not reached within tolerance the stage fails where it stands
    (c2p-madril-v1a kept walking into a rock for 500 ticks per waypoint)."""
    points = madril_plan.WAYPOINTS
    s = c.status()
    need(s['hero_position'] is not None, 'madril-road', 'no field control at walk start', status=s)
    walkable = [i for i, p in enumerate(points) if p[0] != 'confirm']
    i = min(walkable, key=lambda i: (points[i][0] - s['hero_position'][0]) ** 2
            + (points[i][1] - s['hero_position'][2]) ** 2)
    misses = events = 0
    handled = []
    while i < len(points):
        if points[i][0] == 'confirm':
            handled.append(confirm_event(c, run, len(handled), points[i][1:]))
            i += 1
            continue
        x, z = points[i]
        s = c.walk_to(x, z, tol=3, max_ticks=500, stop=lambda s: s['event_running'] or s['scene'] != 1)
        if battle.fight_if_needed(c, run):
            continue
        s = c.status()
        if s['event_running']:
            events += 1
            need(events <= max_events, 'madril-road', 'field events kept interrupting the walk', status=s)
            c.note('field event while walking', s)
            after = c.event_dialogue('madril-road')
            need(not after['event_running'], 'madril-road', 'field event did not finish', status=after)
            handled.append(record_event(c, run, len(handled), s, after))
            continue
        need(s['scene'] == 1 and s['hero_position'] is not None, 'madril-road',
             'left scene 1 or lost field control on the road', status=s, waypoint=i)
        if navigate.near(s, (x, z), 3.5):
            misses = 0
        else:
            misses += 1
            need(misses < max_misses, 'madril-road', 'walk stalled: consecutive waypoints not reached',
                 status=s, waypoint=i, target=(x, z))
        i += 1
    tx, tz = madril_plan.TRIGGER
    s = navigate.navigate(c, run, (tx, tz), tol=3, budget=1500)
    need(navigate.near(s, (tx, tz), 4), 'madril-road', 'event-32 trigger not reached', status=s)
    record('road', {'at_trigger': s, 'waypoints': len(madril_plan.WAYPOINTS), 'crossed_plan': madril_plan.CROSSED,
                    'field_events': handled})
    return s

def record_event(c, run, n, before, after):
    """Variables after a field event met on the road (the route card erratum:
    the bridge NPC's message 20201 writes objective 60 before Madril)."""
    name = f'road-event-{n}'
    c.send(f'capture {name}')
    path = Path(run) / 'control' / f'{name}.png.variables.bin'
    state = progress(path.read_bytes()) if path.is_file() else None
    return {'started': before, 'finished': after, 'message': after.get('message'), 'variables': str(path),
            'state': state}

def confirm_event(c, run, n, toward):
    """Face `toward` with a short ordinary press, Confirm, and run the event's
    dialogue (madril_plan: the bridge-end NPC blocks the road until spoken to)."""
    s = c.status()
    hx, hz = s['hero_position'][0], s['hero_position'][2]
    c.step(4, c.pick(s['camera_yaw'], toward[0] - hx, toward[1] - hz))
    c.step(2, 0)
    c.step(1, CONFIRM)
    c.step(30, 0)
    started = c.status()
    need(started['event_running'], 'madril-road', 'Confirm started no event', status=started, toward=toward)
    after = c.event_dialogue('madril-road')
    need(not after['event_running'], 'madril-road', 'field event did not finish', status=after)
    return record_event(c, run, n, started, after)

def confirm_trigger(c, run, start):
    """Confirm on the trigger polygons (kind 0x80) until the field requests scene 14."""
    for attempt in range(6):
        c.step(2, 0)
        c.step(1, CONFIRM)
        for _ in range(60):
            if TRANSITION in c.log_since(start) or c.status()['scene'] == SCENE:
                return attempt
            if c.observe()['battle']:
                battle.fight_if_needed(c, run)
                break
            c.step(10, 0)
        s = c.status()
        if s['event_running']:
            for _ in range(400):
                if TRANSITION in c.log_since(start) or c.status()['scene'] == SCENE:
                    return attempt
                c.step(10, 0)
        # Re-centre on the trigger before the next Confirm.
        navigate.navigate(c, run, madril_plan.TRIGGER, tol=2, budget=300)
    need(False, 'madril-trigger', 'Confirm on the event-32 trigger did not request scene 14',
         status=c.status(), log_tail=c.log_since(start)[-2000:])

def arrive(c, run, start):
    for _ in range(600):
        s = c.status()
        if s['scene'] == SCENE:
            break
        c.step(10, 0)
    s = c.status()
    need(s['scene'] == SCENE and TRANSITION in c.log_since(start), 'madril-arrival',
         'scene 14 entrance 1 was not loaded by the field transition', status=s)
    return s

def through_message(c, start):
    """Advance the arrival event with ordinary Confirms, recording whether
    message 21020 was the open message at some observed status."""
    seen = []
    def until(s):
        if s.get('message') == MESSAGE and (not seen or seen[-1] != s['frame']):
            seen.append(s['frame'])
        return seen and not s['event_running'] and s['hero_position'] is not None
    s = c.status()
    until(s)
    for _ in range(40):
        if until(s):
            break
        s = c.advance(until, confirm_gap=40, max_ticks=600)
    need(seen, 'madril-message', f'message {MESSAGE} was never the open message', status=s)
    need(not s['event_running'] and s['scene'] == SCENE, 'madril-message',
         'arrival event did not finish in scene 14', status=s)
    return {'message_frames': [seen[0], seen[-1]], 'after': s}

def main(c, run, manifest, slot_file, slot_rights, expect_sha, record, rest_first=True):
    data = restore_check.check_slot(run, manifest, slot_file, expect_sha)
    record('slot', {'slot_file': slot_file, 'expected_save_sha256': expect_sha})
    start = c.log_offset()
    record('restore-route', restore_route.main(c, run, manifest, slot_rights))
    v = (Path(run) / 'control' / 'restored-field.png.variables.bin').read_bytes()
    diff = restore_check.payload_differences(data, v)
    unexpected = [d for d in diff if int(d['offset'], 16) not in restore_check.ALLOWED_PAYLOAD_DIFFERENCES]
    need(not unexpected, 'restore-check', 'restored variables differ from the saved payload', differences=diff)
    entry = progress(v)
    need(entry == {'story': STORY, 'scene_word': 74, 'objective': 10, 'b1_125': 0}, 'restore-check',
         'restored state is not the route card entry state', state=entry)
    record('payload', {'bytes': savefmt.PAYLOAD_SIZE, 'differences': diff, 'entry_state': entry})
    if rest_first:
        landing = restore_check.to_landing(c)
        need(in_field_control(landing, 73), 'rest', 'landing (scene 73) not reached', status=landing)
        record('rest', {'bedroom_menu': home_inside.enter_bedroom(c, run)['bedroom_menu'],
                        'rest': home_inside.rest(c, 'rest0')})
    landing, hall, outside = restore_check.leave_house(c, run)
    restore_check.to_patrol(c, run)
    gate = c.status()
    need(in_field_control(gate, 1), 'madril-road', 'no field control past the gate', status=gate)
    record('transitions', {'landing': landing, 'hall': hall, 'outside': outside, 'gate': gate})
    walk_road(c, run, record)
    attempts = confirm_trigger(c, run, start)
    s = arrive(c, run, start)
    record('arrival', {'confirm_attempts': attempts + 1, 'status': s,
                       'transition': [l for l in c.log_since(start).splitlines() if TRANSITION in l][-1:]})
    msg = through_message(c, start)
    record('message', msg)
    c.send('capture madril-arrival')
    path = Path(run) / 'control' / 'madril-arrival.png.variables.bin'
    for _ in range(100):
        if path.is_file():
            break
        c.step(1, 0)
    need(path.is_file(), 'madril-state', 'variables capture missing')
    state = progress(path.read_bytes())
    need(state == {'story': STORY, 'scene_word': SCENE, 'objective': OBJECTIVE_AFTER, 'b1_125': 1}, 'madril-state',
         'Madril arrival postconditions do not hold', state=state)
    record('state', {'variables': str(path), **state})
    before = c.status()
    c.step(20, UP)
    c.step(1, 0)
    after = c.status()
    if not hero_moved(before, after):
        c.step(20, RIGHT)
        c.step(1, 0)
        after = c.status()
    guards = guard_lines(c.log_since(start))
    need(after['scene'] == SCENE and not after['event_running'] and hero_moved(before, after), 'madril-control',
         'no ordinary movement in scene 14 after the arrival event', before=before, after=after)
    need(not guards, 'madril-control', 'guard or error lines in the native log', lines=guards[:20])
    need(c.alive(), 'madril-control', 'native process not alive')
    record('control', {'before': before, 'after': after})
