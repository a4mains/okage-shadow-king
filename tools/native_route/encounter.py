"""Patrol with ordinary movement until a natural kumo appears, then approach it.
No formation, RNG or outcome is chosen; contact and formation come from the game.

Success requires the native log to report `Native natural encounter frame=`
after the patrol began, and then either the battle/mode change or a field freed
after that contact (the battle stage then requires mode 3). Budget exhaustion,
or a field that disappears without a contact since the patrol began, is a
failure.
"""
import math

from .battle import CONTACT
from .outcome import need

def main(c, run, a=(-160, 200), b=(-160, 150), budget=6000):
    start = c.log_offset()
    used = 0
    target = b
    while True:
        need(used < budget, 'encounter', 'patrol budget exhausted without a natural contact', budget=budget)
        if c.roar_active():
            used += c.roar()
        o = c.observe()
        if o['battle'] or o['game_mode'] != 2:
            c.note('MODE', o['game_mode'], o['battle'])
            break
        kumo = [x for x in o['actors'] if 'kumo' in x['model'] and x['visible']]
        s = c.status()
        if s['scene'] is None and any(l.startswith(CONTACT) for l in c.log_since(start).splitlines()):
            c.note('field freed after contact', o['game_mode'], 'frame', o['observe_frame'])
            break
        need(s['scene'] is not None and s['hero_position'] is not None, 'encounter',
             'field gone without a battle', status=s, observe_mode=o['game_mode'])
        hx, hz = s['hero_position'][0], s['hero_position'][2]
        if kumo:
            k = min(kumo, key=lambda x: math.hypot(x['position'][0] - hx, x['position'][2] - hz))
            d = math.hypot(k['position'][0] - hx, k['position'][2] - hz)
            c.note('kumo', k['alias'], k['model'], [round(v, 1) for v in k['position']], 'dist', round(d, 1), 'frame', s['frame'])
            c.step(8, c.pick(s['camera_yaw'], k['position'][0] - hx, k['position'][2] - hz))
            used += 8
            continue
        if math.hypot(target[0] - hx, target[1] - hz) < 4:
            target = a if target == b else b
        c.step(10, c.pick(s['camera_yaw'], target[0] - hx, target[1] - hz))
        used += 10
    text = c.log_since(start)
    contact = [l for l in text.splitlines() if l.startswith(CONTACT)]
    need(len(contact) >= 1, 'encounter', 'mode changed but no native natural-encounter line',
         observe_mode=o['game_mode'], battle=o['battle'])
    return {'contact_line': contact[0], 'ticks': used, 'observe_frame': o['observe_frame']}
