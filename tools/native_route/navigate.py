"""Greedy ordinary-input navigation: from each position try the 8 pad directions
ranked by progress toward a target, take short steps, avoid revisiting 4-unit
cells, fight natural contacts. Only status/observe are read; only pad masks sent.

navigate() returns the last status (target reached, scene changed, or budget
spent); callers must check it. settle() fails instead of looping forever.
"""
import math

from . import battle
from .outcome import need
from .pilot import DIRS

def settle(c, run, scene, max_polls=2000):
    """Scene None: a battle (fight it) or a transition (return the new status)."""
    for _ in range(max_polls):
        if c.roar_active():
            battle.fight_if_needed(c, run)
            continue
        s = c.status()
        if s['scene'] == scene and s['hero_position'] is not None:
            return None
        o = c.observe()
        if o['battle']:
            battle.fight_if_needed(c, run)
            continue
        if s['scene'] is not None and s['scene'] != scene:
            return s
        c.step(10, 0)
    need(False, 'navigate', 'field never settled', scene=scene)

def navigate(c, run, target, tol=4.0, budget=3000, step=8, scene=1):
    tx, tz = target
    visited = {}
    used = 0
    trail = []
    navigate.trail = trail
    while used < budget:
        t = settle(c, run, scene)
        if t:
            return t
        s = c.status()
        hx, hz = s['hero_position'][0], s['hero_position'][2]
        if not trail or math.hypot(trail[-1][0] - hx, trail[-1][1] - hz) > 6:
            trail.append((round(hx, 1), round(hz, 1)))
        if math.hypot(tx - hx, tz - hz) <= tol:
            return s
        cell = (round(hx / 4), round(hz / 4))
        visited[cell] = visited.get(cell, 0) + 1
        fwd = 90.0 - s['camera_yaw'] * 360.0
        options = []
        for off, mask in DIRS:
            a = math.radians(fwd + off)
            dx, dz = math.cos(a), math.sin(a)
            nx, nz = hx + dx * 3, hz + dz * 3
            gain = math.hypot(tx - hx, tz - hz) - math.hypot(tx - nx, tz - nz)
            options.append((gain - 2.0 * visited.get((round(nx / 4), round(nz / 4)), 0), mask))
        options.sort(reverse=True)
        moved = False
        for _, mask in options[:5]:
            c.step(step, mask)
            used += step
            n = c.status()
            if n['scene'] != scene or n['hero_position'] is None:
                moved = True
                break
            if math.hypot(n['hero_position'][0] - hx, n['hero_position'][2] - hz) > 1.0:
                moved = True
                break
        if not moved:
            visited[cell] += 3
    return c.status()

def near(s, target, tol):
    return (s is not None and s.get('hero_position') is not None
            and math.hypot(s['hero_position'][0] - target[0], s['hero_position'][2] - target[1]) <= tol)
