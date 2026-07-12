"""From the house door (scene 70) climb to Ari's room and Save with ordinary input.
Stairs: HOME1A floor contact ~(17.5, 20) -> scene 73; HOME2A contact ~(-13, 7.5) -> 74.
Bedroom startup message 10110: Confirm, Confirm, Down x3 (first arms), Confirm (Save),
slot cursor default, Confirm, Left (Yes) when occupied, Confirm. Nothing is injected.

Save success is never assumed. verify_save() requires all of:
  * the target file in RUN/saves differs from its pinned initial hash (and from
    its hash when this stage began), is 3460 bytes, '!crc', block 0xd78 and its
    checksum verifies (src/save_store.cpp envelope);
  * every other save is unchanged and no other file appeared in RUN/saves;
  * the native log since the Save inputs shows exactly one
    `Native field Save constructed frame=A` followed by one
    `Native field Save retired frame=B` with A < B (v19a: 93068 -> 93471);
  * the native process is still alive;
  * status after the Save is field control in the bedroom (scene 74, no event,
    hero present) and an ordinary Right step moves the hero.
"""
import re
from pathlib import Path

from . import navigate, pins, savefmt
from .outcome import need, run_stage
from .pilot import CONFIRM, DOWN, LEFT, RIGHT, hero_moved, in_field_control

BEDROOM_SCENE = 74
LANDING_SCENE = 73
HOUSE_SCENE = 70
SAVE_CONSTRUCTED = re.compile(r'^Native field Save constructed frame=(\d+)$')
SAVE_RETIRED = re.compile(r'^Native field Save retired frame=(\d+)$')

def settle_events(c):
    s = c.status()
    if s['event_running']:
        s = c.advance(lambda s: not s['event_running'], confirm_gap=60)
    return s

def wait_scene(c, scene, polls=80):
    for _ in range(polls):
        s = c.status()
        if s['scene'] == scene:
            return s
        c.step(10, 0)
    return c.status()

def climb(c, run, tag):
    """House door -> bedroom with the startup menu (message 10110) running."""
    for _ in range(200):
        if c.status()['scene'] is not None:
            break
        c.step(10, 0)
    s = settle_events(c)
    need(in_field_control(s, HOUSE_SCENE), 'home-inside', 'not in the house with field control', status=s)
    c.note('house', s)
    c.send(f'view {tag}-house')
    for t in ((0, -6), (0, 4), (0, 12), (0, 16), (10, 19), (16.5, 17.6)):
        s = navigate.navigate(c, run, t, tol=2.0, budget=500, step=6, scene=HOUSE_SCENE)
        if s['scene'] != HOUSE_SCENE:
            break
    c.interact((16.5, 17.6), (16.5, 24), tol=1.0, advance=False)
    s = wait_scene(c, LANDING_SCENE)
    c.note('landing', s)
    need(s['scene'] == LANDING_SCENE, 'home-inside', 'landing (scene 73) not reached', status=s)
    return enter_bedroom(c, run)

def enter_bedroom(c, run):
    """Landing (scene 73) -> bedroom with the startup menu (message 10110) running."""
    navigate.navigate(c, run, (-12, 4.7), tol=1.5, budget=800, step=6, scene=LANDING_SCENE)
    c.interact((-12, 4.7), (-12, 12), tol=1.0, advance=False)
    for _ in range(60):
        s = c.status()
        if s['scene'] == BEDROOM_SCENE and s['event_running'] and s['pc'] == 2952:
            break
        c.step(10, 0)
    c.note('bedroom menu event', s)
    need(s['scene'] == BEDROOM_SCENE and s['event_running'], 'home-inside', 'bedroom startup menu not running', status=s)
    return {'bedroom_menu': s}

REST_RECOVERED = 'Native field Rest recovered party frame='

def rest(c, tag, settle_ticks=1500):
    """Bedroom startup menu (message 10110) running: the same ordinary inputs as
    the Save stage up to the cursor, whose first Down arms it on Rest (choice 0
    of Rest / Change party / Save), then Confirm. The authored Rest event runs
    (opcode 0x62 request 10, the native Rest route). Pass requires the cursor on
    Rest, a native party-recovery line after the Confirm and bedroom field
    control afterwards. Nothing is injected; HP comes only from the event."""
    c.step(150, 0)
    c.step(1, CONFIRM)
    c.step(60, 0)
    c.step(1, CONFIRM)
    c.step(45, 0)
    c.step(1, DOWN)
    c.step(20, 0)
    s = c.status()
    c.note('menu choice', s['choice'])
    c.send(f'view {tag}-menu-rest')
    need(s['choice'] == 0, 'rest', 'menu cursor not on Rest', status=s)
    start = c.log_offset()
    c.step(1, CONFIRM)
    for _ in range(settle_ticks // 10):
        if in_field_control(c.status(), BEDROOM_SCENE):
            break
        c.step(10, 0)
    s = c.status()
    if not in_field_control(s, BEDROOM_SCENE):
        s = c.advance(lambda s: in_field_control(s, BEDROOM_SCENE), confirm_gap=60, max_ticks=3000)
    recovered = [l for l in c.log_since(start).splitlines() if l.startswith(REST_RECOVERED)]
    c.send(f'view {tag}-after-rest')
    need(recovered, 'rest', 'no native party recovery after choosing Rest', status=s)
    need(in_field_control(s, BEDROOM_SCENE), 'rest', 'not back in bedroom field control after Rest', status=s)
    return {'recovered': recovered, 'after': s}

def saves_snapshot(run):
    d = Path(run) / 'saves'
    return {p.name: pins.sha256(p) for p in sorted(d.iterdir()) if p.is_file()} if d.is_dir() else {}

def save_inputs(c, tag):
    """The historical ordinary menu inputs (unchanged waits). Returns statuses."""
    c.step(150, 0)
    c.send(f'view {tag}-menu0')
    c.step(1, CONFIRM)
    c.step(60, 0)
    c.step(1, CONFIRM)
    c.step(45, 0)
    for _ in range(3):
        c.step(1, DOWN)
        c.step(20, 0)
    s = c.status()
    c.note('menu choice', s['choice'])
    c.send(f'view {tag}-menu-save')
    need(s['choice'] == 2, 'save', 'menu cursor not on Save', status=s)
    c.step(1, CONFIRM)
    c.step(160, 0)
    c.send(f'view {tag}-slots')
    c.step(1, CONFIRM)
    c.step(60, 0)
    c.send(f'view {tag}-replace')
    c.step(1, LEFT)
    c.step(20, 0)
    c.send(f'view {tag}-replace-yes')
    c.step(1, CONFIRM)
    c.step(420, 0)
    after = c.status()
    c.note('after save', after)
    c.send(f'capture {tag}-after-save', f'view {tag}-after-save-view')
    c.step(20, RIGHT)
    c.step(1, 0)
    moved = c.status()
    c.note('moved', moved)
    return after, moved

def verify_save(run, manifest, target, before, log_segment, after, moved, alive):
    """Return (evidence, reasons). Success iff reasons is empty."""
    run = Path(run)
    reasons = []
    pinned = (manifest.get('initial_saves') or {}).get(target)
    path = run / 'saves' / target
    evidence = {'target': target, 'pinned_initial_sha256': pinned and pinned['sha256'],
                'stage_start_sha256': before.get(target)}
    if not path.is_file():
        reasons.append('target save file missing')
        data = b''
    else:
        data = path.read_bytes()
        evidence['sha256'] = pins.sha256(path)
        evidence['size'] = len(data)
        fmt = savefmt.problems(data)
        evidence['format_problems'] = fmt
        if fmt:
            reasons.append('target save malformed: ' + '; '.join(fmt))
        if pinned and evidence['sha256'] == pinned['sha256']:
            reasons.append('target save unchanged from its pinned initial hash')
        if before.get(target) is not None and evidence['sha256'] == before[target]:
            reasons.append('target save unchanged during the Save stage')
    now = saves_snapshot(run)
    for name in sorted(set(now) | set(before)):
        if name == target:
            continue
        if name not in before:
            reasons.append('unexpected new file in saves: ' + name)
        elif name not in now:
            reasons.append('save file disappeared: ' + name)
        elif now[name] != before[name]:
            reasons.append('non-target save changed: ' + name)
    for name, pin in sorted((manifest.get('initial_saves') or {}).items()):
        if name != target and now.get(name) != pin['sha256']:
            reasons.append('non-target save differs from its pinned initial hash: ' + name)
    lines = log_segment.splitlines()
    built = [(i, int(m.group(1))) for i, l in enumerate(lines) for m in [SAVE_CONSTRUCTED.match(l)] if m]
    retired = [(i, int(m.group(1))) for i, l in enumerate(lines) for m in [SAVE_RETIRED.match(l)] if m]
    evidence['log'] = {'constructed': [f for _, f in built], 'retired': [f for _, f in retired]}
    if len(built) != 1 or len(retired) != 1:
        reasons.append(f'expected one Save constructed and one retired line, got {len(built)} and {len(retired)}')
    elif not (built[0][0] < retired[0][0] and built[0][1] < retired[0][1]):
        reasons.append('Save retired line does not follow its construction')
    evidence['alive'] = bool(alive)
    if not alive:
        reasons.append('native process not alive after the Save')
    evidence['after'] = after
    evidence['moved'] = moved
    if not (after and in_field_control(after, BEDROOM_SCENE)):
        reasons.append('not back in bedroom field control after the Save')
    if not hero_moved(after, moved):
        reasons.append('ordinary Right step did not move the hero after the Save')
    return evidence, reasons

def save(c, run, manifest, target, tag):
    before = saves_snapshot(run)
    pinned = (manifest.get('initial_saves') or {}).get(target)
    need(pinned is None or before.get(target) == pinned['sha256'], 'save',
         'target save changed before the Save stage', target=target)
    start = c.log_offset()
    after, moved = save_inputs(c, tag)
    evidence, reasons = verify_save(run, manifest, target, before, c.log_since(start), after, moved, c.alive())
    need(not reasons, 'save', 'Save postconditions failed', reasons=reasons, evidence=evidence)
    return evidence

def main(c, run, manifest, target, tag='strict', record=None):
    record = record or (lambda name, ev: None)
    record('home-inside', run_stage('home-inside', climb, c, run, tag))
    record('save', run_stage('save', save, c, run, manifest, target, tag))
