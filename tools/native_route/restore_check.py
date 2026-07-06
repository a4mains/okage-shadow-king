"""Fresh-process verification of a genuine Save: ordinary title Restore of a
slot, payload/variables comparison, ordinary transitions out of the house and a
natural encounter fought to victory.

The restored save is read only from RUN/saves (pinned by prepare from the
strict run's saves). Pass requires every check below; restore-check.json is
written with status PASS/FAIL and the exit code is nonzero on any failure:
  * the slot file is pinned, unchanged, well formed, and (optionally) equals
    --expect-save-sha256 (the Save run's verified output hash);
  * the restored variables equal the saved payload except the documented
    playtime byte 0x520 (ALLOWED_PAYLOAD_DIFFERENCES);
  * an ordinary Right step moves the hero; ordinary transitions reach the
    landing (73), the hall (70) and outside (1);
  * a natural encounter is fought to a native victory and field control returns;
  * the native process is alive before the final quit.
"""
import math
from pathlib import Path

from . import battle, home, home_inside, navigate, pins, restore_route, savefmt
from .outcome import need
from .pilot import CONFIRM, RIGHT, hero_moved, in_field_control

ALLOWED_PAYLOAD_DIFFERENCES = {0x520: 'playtime (the native Save contract)'}

def payload_differences(save_bytes, variables_bytes):
    payload = savefmt.payload(save_bytes)
    need(len(variables_bytes) >= len(payload), 'restore-check', 'variables capture shorter than the payload',
         size=len(variables_bytes))
    return [{'offset': hex(i), 'saved': payload[i], 'restored': variables_bytes[i]}
            for i in range(len(payload)) if payload[i] != variables_bytes[i]]

def check_slot(run, manifest, slot_file, expect_sha=None):
    pinned = (manifest.get('initial_saves') or {}).get(slot_file)
    need(pinned is not None, 'restore-check', 'slot file is not a pinned initial save', slot_file=slot_file)
    path = Path(run) / 'saves' / slot_file
    need(path.is_file() and pins.sha256(path) == pinned['sha256'], 'restore-check', 'slot file changed in the restore run')
    data = path.read_bytes()
    need(not savefmt.problems(data), 'restore-check', 'slot file malformed', problems=savefmt.problems(data))
    if expect_sha is not None:
        need(pinned['sha256'] == expect_sha, 'restore-check', 'slot file is not the expected saved output',
             pinned=pinned['sha256'], expected=expect_sha)
    return data

def leave_house(c, run, stage='restore-check'):
    """Bedroom field control -> landing (73) -> hall (70) -> outside (1), ordinary input."""
    c.walk_to(4.38, -11.85, tol=0.6)
    for _ in range(6):
        if c.status()['scene'] == 73:
            break
        c.step(1, CONFIRM)
        c.step(40, 0)
    for _ in range(200):
        if c.status()['scene'] == 73:
            break
        c.step(10, 0)
    landing = c.status()
    need(in_field_control(landing, 73), stage, 'landing (scene 73) not reached', status=landing)
    navigate.navigate(c, run, (18.2, -0.7), tol=1.5, budget=800, step=6, scene=73)
    c.walk_to(20.3, -1.3, tol=0.8, max_ticks=60)
    c.interact((20.3, -1.3), (26, -6), tol=0.8, advance=False)
    for _ in range(60):
        if c.status()['scene'] == 70:
            break
        c.step(10, 0)
    hall = c.status()
    need(in_field_control(hall, 70), stage, 'hall (scene 70) not reached', status=hall)
    for t in ((10, 19), (0, 16), (0, 4), (0, -6), (-0.3, -24)):
        navigate.navigate(c, run, t, tol=2.0, budget=500, step=6, scene=70)
    c.interact((-0.3, -26.5), (-0.3, -35), tol=1.0, advance=False)
    for _ in range(80):
        if c.status()['scene'] == 1:
            break
        c.step(10, 0)
    c.step(30, 0)
    s = c.status()
    if s['event_running']:
        c.advance(lambda s: not s['event_running'], confirm_gap=60, max_ticks=6000)
    outside = c.status()
    need(in_field_control(outside, 1), stage, 'outside (scene 1) not reached', status=outside)
    return landing, hall, outside

def to_patrol(c, run):
    """Outside the house door -> through the yard gate to the patrol start."""
    navigate.navigate(c, run, (192, 711), tol=3, budget=2000)
    s = navigate.navigate(c, run, (176, 712), tol=3, budget=600)
    if s['hero_position'] is None or math.hypot(s['hero_position'][0] - 176, s['hero_position'][2] - 712) > 5:
        c.interact((190, 711), (170, 711), tol=1.5)
        navigate.navigate(c, run, (176, 712), tol=3, budget=600)

def main(c, run, manifest, slot_file, slot_rights, expect_sha=None, record=None, until_roar=0, rest_every=0):
    record = record or (lambda name, ev: None)
    data = check_slot(run, manifest, slot_file, expect_sha)
    record('slot', {'slot_file': slot_file, 'expected_save_sha256': expect_sha,
                    'pinned_sha256': (manifest.get('initial_saves') or {}).get(slot_file, {}).get('sha256')})
    start = c.log_offset()
    record('restore-route', restore_route.main(c, run, manifest, slot_rights))
    v = (Path(run) / 'control' / 'restored-field.png.variables.bin').read_bytes()
    diff = payload_differences(data, v)
    unexpected = [d for d in diff if int(d['offset'], 16) not in ALLOWED_PAYLOAD_DIFFERENCES]
    need(not unexpected, 'restore-check', 'restored variables differ from the saved payload', differences=diff)
    record('payload', {'bytes': savefmt.PAYLOAD_SIZE, 'differences': diff,
                       'allowed': {hex(k): v for k, v in ALLOWED_PAYLOAD_DIFFERENCES.items()}})
    restored = c.status()
    c.step(20, RIGHT)
    c.step(1, 0)
    moved = c.status()
    need(hero_moved(restored, moved), 'restore-check', 'ordinary Right step did not move the restored hero')
    landing, hall, outside = leave_house(c, run)
    record('transitions', {'moved': moved['hero_position'], 'landing': landing, 'hall': hall, 'outside': outside})
    to_patrol(c, run)
    for leg in range(16):
        navigate.navigate(c, run, (120, 735) if leg % 2 == 0 else (170, 713), tol=4, budget=500)
        if battle.VICTORY in c.log_since(start):
            break
    text = c.log_since(start).splitlines()
    encounters = [l for l in text if battle.CONTACT in l or battle.VICTORY in l]
    contact = [i for i, l in enumerate(text) if l.startswith(battle.CONTACT)]
    victory = [i for i, l in enumerate(text) if l.startswith(battle.VICTORY)]
    need(contact and victory and contact[0] < victory[0], 'restore-check',
         'no natural encounter fought to victory after Restore', encounters=encounters)
    after = c.status()
    need(in_field_control(after, 1), 'restore-check', 'no field control after the encounter', status=after)
    need(c.alive(), 'restore-check', 'native process not alive at the end of the check')
    record('encounter', {'after_encounter': after, 'encounters': encounters})
    if until_roar:
        record('roar-encounters', roar_encounters(c, run, until_roar, rest_every))
    c.send('quit')

ROAR = 'Native natural encounter message id='

def rest_cycle(c, run, n):
    """Ordinary walk home from the patrol, climb to the bedroom, choose Rest,
    then leave the house and return to the patrol start (all ordinary input;
    each step has its own verified postcondition)."""
    tag = f'rest{n}'
    ev = {'house': home.main(c, run, from_gate=True)['house']}
    ev['bedroom_menu'] = home_inside.climb(c, run, tag)['bedroom_menu']
    ev['rest'] = home_inside.rest(c, tag)
    landing, hall, outside = leave_house(c, run, stage='rest')
    ev['outside'] = outside
    to_patrol(c, run)
    return ev

def roar_encounters(c, run, max_victories, rest_every=0):
    """Keep the same ordinary patrol (and the battle/roar handling navigate
    already does) until a natural encounter that opened Stan's phase-8 roar
    message is fought to victory, at most `max_victories` further victories.
    With `rest_every` N, after every N victories without a won roar the party
    walks home and Rests in the bedroom (the ordinary heal; no state edit), then
    resumes the same patrol. Every contact, roar, victory and Rest is recorded;
    nothing is selected. The roar itself depends on playtime at contact
    (0x40004 = variables+0x520, seconds; src/enemy.cpp), which is never steered."""
    start = c.log_offset()
    rests = []
    rested_at = 0
    for leg in range(40 * max_victories):
        navigate.navigate(c, run, (120, 735) if leg % 2 == 0 else (170, 713), tol=4, budget=500)
        lines = c.log_since(start).splitlines()
        victories = [i for i, l in enumerate(lines) if l.startswith(battle.VICTORY)]
        roars = [i for i, l in enumerate(lines) if l.startswith(ROAR)]
        if any(v > r for r in roars for v in victories) or len(victories) >= max_victories:
            break
        if rest_every and len(victories) - rested_at >= rest_every:
            rests.append(rest_cycle(c, run, len(rests)))
            rested_at = len([l for l in c.log_since(start).splitlines() if l.startswith(battle.VICTORY)])
    lines = c.log_since(start).splitlines()
    events = [l for l in lines if l.startswith(battle.CONTACT) or l.startswith(ROAR) or l.startswith(battle.VICTORY)
              or 'music variant' in l or l.startswith(home_inside.REST_RECOVERED)]
    roars = [i for i, l in enumerate(lines) if l.startswith(ROAR)]
    victories = [i for i, l in enumerate(lines) if l.startswith(battle.VICTORY)]
    need(roars and any(v > roars[0] for v in victories), 'roar-encounters',
         'no phase-8 roar encounter fought to victory within the budget', events=events, rests=len(rests))
    after = c.status()
    need(in_field_control(after, 1), 'roar-encounters', 'no field control after the roar battle', status=after)
    return {'events': events, 'rests': rests, 'after': after}
