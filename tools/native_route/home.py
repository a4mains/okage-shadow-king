"""Ordinary walk from the Chapter 2 encounter area back to the house door
(FIELD_1D contact at ~287,689, minimap east path), fighting natural contacts.
Success requires the house interior (scene 70) with field control; every battle
on the way must reach a verified victory (battle.fight_if_needed)."""
from . import battle, navigate
from .outcome import need
from .pilot import in_field_control

WAYPOINTS = [(-158, 241), (-163, 252), (-161, 317), (-142, 318), (-155, 353), (-162, 406), (-175, 458), (-187, 461),
             (-184, 479), (-194, 506), (-184, 537), (-176, 552), (-161, 571), (-138, 565), (-159, 624), (-152, 672), (-178, 660), (-183, 645),
             (-168, 684), (-178, 725), (-172, 742), (-164, 741), (-118, 753), (-128, 780), (-101, 824),
             (-81, 828), (-76, 844), (-58, 856),
             # claude-dev-d13: the dirt path turns east along the fences.
             (-54, 853), (-39, 846), (-16, 845), (-1, 842), (12, 840), (10, 825), (17, 810), (38, 800),
             # claude-dev-d13/claude-strict-v15b/v19a: through the fenced bend, then east to entrance 11.
             (39.6, 787), (44, 773), (56, 762), (68.5, 751), (76.6, 756.4), (84.3, 756.9), (91.7, 755),
             (96.8, 749.2), (103.3, 743.3), (107.9, 737.1), (115.6, 734.4), (122.2, 728.6), (135.4, 723.8),
             (148.7, 719.6), (164, 713), (176, 712)]
NAV_TARGETS = [(178, 711), (283, 690)]
YARD = [(197.6, 712.8), (212.9, 704.3), (227.3, 697.0), (238.8, 701.0), (249.1, 709.1), (263.1, 699.4),
        (274.1, 688.6)]
HOUSE_SCENE = 70

def walk_points(c, run, points, scene, max_events=50):
    s = c.status()
    need(s['hero_position'] is not None, 'home', 'no field control at walk start', status=s)
    i = min(range(len(points)), key=lambda i: (points[i][0] - s['hero_position'][0]) ** 2 + (points[i][1] - s['hero_position'][2]) ** 2)
    events = 0
    while i < len(points):
        x, z = points[i]
        c.walk_to(x, z, tol=3, max_ticks=500, stop=lambda s: s['event_running'] or s['scene'] != scene)
        if battle.fight_if_needed(c, run):
            continue
        s = c.status()
        if s['event_running']:
            events += 1
            need(events <= max_events, 'home', 'field events kept interrupting the walk')
            c.note('field event while walking', s)
            c.advance(lambda s: not s['event_running'], confirm_gap=60)
            continue
        if s['scene'] != scene:
            return s
        i += 1
    return c.status()

def main(c, run, from_gate=False):
    battle.fight_if_needed(c, run)
    if not from_gate:
        walk_points(c, run, WAYPOINTS, 1)
    s = navigate.navigate(c, run, (178, 711))
    c.note('navigate gate approach ->', s)
    s = c.interact((181, 711), (195, 711))
    c.note('gate ->', s)
    walk_points(c, run, YARD, 1)
    s = navigate.navigate(c, run, (277, 687), tol=3, budget=1500)
    c.note('navigate door ->', s)
    for _ in range(3):
        if c.status()['scene'] != 1:
            break
        s = c.interact((285.6, 689.4), (295, 689.4), tol=1.0)
        c.note('door ->', s)
    for _ in range(60):
        s = c.status()
        if s['scene'] is not None and s['scene'] != 1:
            break
        if c.observe()['battle']:
            battle.fight_if_needed(c, run)
            continue
        c.step(10, 0)
    s = c.status()
    c.note('end', s)
    need(in_field_control(s, HOUSE_SCENE), 'home', 'house interior (scene 70) not reached', status=s)
    return {'house': s}
