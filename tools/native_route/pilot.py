"""Closed-loop ordinary-pad helper for a session_driver run.

Every action is an ordinary `step TICKS MASK` request appended to commands.in
and journaled by the driver. Decisions use only the documented read-only
`status` and `observe` outputs (and, in battle.py, battle-live observation
rows). Nothing writes game state, seeds, timing or outcomes.
"""
import json
import math
import os
import time
from pathlib import Path

from .outcome import need

UP, RIGHT, DOWN, LEFT, CONFIRM, CANCEL, START = 0x1000, 0x2000, 0x4000, 0x8000, 0x0040, 0x0020, 0x0800
DIRS = [(0, UP), (-90, RIGHT), (180, DOWN), (90, LEFT), (-45, UP | RIGHT), (45, UP | LEFT),
        (-135, DOWN | RIGHT), (135, DOWN | LEFT)]

class NativeExited(RuntimeError):
    """The native process or its driver ended before acknowledging a request."""

class ProtocolError(RuntimeError):
    """The driver rejected a request or an expected observation is missing."""

def pid_alive(pid):
    try:
        os.kill(int(pid), 0)
        return True
    except (OSError, TypeError, ValueError):
        return False

class FileTransport:
    """commands.in -> driver -> native; responses read from stdout.log."""

    def __init__(self, run):
        self.run = Path(run)
        self.log = self.run / 'stdout.log'
        proc = json.loads((self.run / 'process.json').read_text())
        self.native_pid, self.driver_pid = proc['native_pid'], proc['driver_pid']

    def _state(self):
        for _ in range(200):
            try:
                return json.loads((self.run / 'driver-state.json').read_text())
            except (OSError, ValueError):
                if not self.alive():
                    raise NativeExited('driver state unavailable and process ended')
                time.sleep(0.05)
        raise ProtocolError('driver-state.json unreadable')

    def alive(self):
        return pid_alive(self.native_pid) and pid_alive(self.driver_pid)

    def log_size(self):
        return self.log.stat().st_size

    def log_text(self, start=0):
        with self.log.open('rb') as f:
            f.seek(start)
            return f.read().decode(errors='replace')

    def send(self, cmds):
        start = self._state()
        pos = self.log_size()
        with (self.run / 'commands.in').open('a') as f:
            for c in cmds:
                f.write(c + '\n')
        while True:
            s = self._state()
            if s['consumed'] >= start['consumed'] + len(cmds):
                break
            if not self.alive() or not s.get('alive', True):
                time.sleep(0.5)
                s = self._state()
                if s['consumed'] >= start['consumed'] + len(cmds):
                    break
                out = self.log_text(pos).splitlines()
                raise NativeExited('native exited:\n' + '\n'.join(out[-15:]))
            time.sleep(0.02)
        out = self.log_text(pos).splitlines()
        if s['rejected'] != start['rejected']:
            raise ProtocolError('driver rejected request(s) ' + repr(cmds) + ': ' + repr(out[-3:]))
        if not s.get('alive', True) and cmds[-1] != 'quit':
            raise NativeExited('native exited:\n' + '\n'.join(out[-15:]))
        return out

class Ctl:
    def __init__(self, run, verbose=True, transport=None):
        self.run = Path(run)
        self.verbose = verbose
        self.t = transport if transport is not None else FileTransport(run)

    # ---- protocol ----
    def alive(self):
        return self.t.alive()

    def log_offset(self):
        return self.t.log_size()

    def log_since(self, offset):
        return self.t.log_text(offset)

    def send(self, *cmds):
        return self.t.send(list(cmds))

    @staticmethod
    def _json(lines, key):
        for line in reversed(lines):
            if line.startswith('{') and key in line:
                return json.loads(line)
        raise ProtocolError('no json ' + key + ' in ' + repr(lines[-5:]))

    def status(self):
        return self._json(self.send('status'), '"frame"')

    def observe(self):
        return self._json(self.send('observe'), '"observe_frame"')

    def step(self, n, mask=0):
        while n > 0:
            k = min(n, 6000)
            self.send(f'step {k} {mask:04x}')
            n -= k

    def tap(self, mask, hold=4, after=0):
        self.step(hold, mask)
        if after:
            self.step(after, 0)

    def note(self, *a):
        if self.verbose:
            print(*a, flush=True)

    # ---- movement ----
    @staticmethod
    def pick(yaw, dx, dz, exclude=()):
        target = math.degrees(math.atan2(dz, dx))
        fwd = 90.0 - yaw * 360.0  # calibrated on scene70 and road
        best = None
        for off, mask in DIRS:
            if mask in exclude:
                continue
            d = (target - (fwd + off) + 180) % 360 - 180
            if best is None or abs(d) < best[0]:
                best = (abs(d), mask)
        return best[1]

    def walk_to(self, x, z, tol=1.0, chunk=6, max_ticks=1500, stop=None):
        """Best-effort walk; callers must check the returned status."""
        used = 0
        stalls = 0
        exclude = []
        s = self.status()
        scene = s['scene']
        while used < max_ticks:
            if s['hero_position'] is None:
                return s
            hx, hz = s['hero_position'][0], s['hero_position'][2]
            dx, dz = x - hx, z - hz
            dist = math.hypot(dx, dz)
            if dist <= tol:
                break
            if stop and stop(s):
                break
            mask = self.pick(s['camera_yaw'], dx, dz, exclude)
            k = max(2, min(chunk, int(dist / 0.4)))
            self.step(k, mask)
            used += k
            n = self.status()
            if n['scene'] != scene or n['event_running'] or n['hero_position'] is None:
                s = n
                break
            moved = math.hypot(n['hero_position'][0] - hx, n['hero_position'][2] - hz)
            if moved < 0.15 * k:
                stalls += 1
                exclude.append(mask)
                if stalls >= 6:
                    s = n
                    self.note('walk_to stalled')
                    break
                if len(exclude) > 3:
                    exclude = exclude[-3:]
            else:
                exclude = []
                stalls = 0
            s = n
        if s['hero_position'] is None:
            return s
        self.step(1, 0)
        s = self.status()
        if s['hero_position'] is None:
            return s
        self.note('walk_to', (x, z), '->', [round(v, 2) for v in s['hero_position']], 'scene', s['scene'], 'used', used)
        return s

    # ---- natural-encounter roar (phase 8, messages 100001..100013) ----
    def roar_active(self):
        text = self.t.log_text(0)
        m = text.rfind('Native natural encounter message id=')
        return m >= 0 and text.rfind('Native battle', m) < 0 and text.rfind('Native natural encounter frame=', m) < 0

    def event_dialogue(self, stage, gap=40, max_ticks=20000, rearm_after=8):
        """Ordinary input through a running field event's dialogue, including
        CC choice menus. CC arms like the roar's QC wheels (0x148dc8: while
        unarmed Up/Down only arm, Confirm needs the armed state), so the same
        rule applies: Confirm; when the text did not advance, one Down arms and
        the next Confirm takes the default row. A Down that moves the cursor
        fails `stage`. Returns the final status (event finished or budget)."""
        def active():
            s = self.status()
            return s['event_running'] and s['scene'] is not None and not self.observe()['battle']
        dialogue(self, active, stage, gap, max_ticks, rearm_after)
        return self.status()

    def roar(self, gap=40, max_ticks=20000, rearm_after=8):
        """Ordinary input through Stan's roar message. Each QC word wheel ignores
        Confirm until Up/Down arms it, and the arming press does not move the
        cursor (the recovered Stan roar dialogue model; claude-final-restore-roar-v2d
        stalled at text_progress 2 on Confirm alone). Confirm; when the text did
        not advance, one Down arms the wheel and the next Confirm takes its
        default row. A Down that moves the cursor fails the stage, so no
        answer is ever chosen."""
        used = dialogue(self, self.roar_active, 'roar-encounters', gap, max_ticks, rearm_after)
        self.note('roar handled after', used, 'ticks')
        return used

    def interact(self, at, toward, tol=1.5, advance=True):
        """Walk to `at`, face `toward` with a short ordinary press, then Confirm and
        let any event run with ordinary Confirms. Callers check the result."""
        s = self.walk_to(at[0], at[1], tol=tol, max_ticks=300)
        if s['hero_position'] is None:
            return s
        hx, hz = s['hero_position'][0], s['hero_position'][2]
        self.step(4, self.pick(s['camera_yaw'], toward[0] - hx, toward[1] - hz))
        self.step(2, 0)
        self.step(1, CONFIRM)
        self.step(30, 0)
        s = self.status()
        if advance and s['event_running']:
            s = self.advance(lambda s: not s['event_running'] or s['scene'] is None, confirm_gap=60)
        return s

    # ---- dialogue ----
    def advance(self, until, confirm_gap=40, max_ticks=20000, hold=4):
        """Press Confirm at a fixed cadence until predicate(status) is true.
        Returns the last status; callers must check the predicate themselves."""
        used = 0
        s = self.status()
        while not until(s) and used < max_ticks:
            self.step(hold, CONFIRM)
            self.step(confirm_gap, 0)
            used += hold + confirm_gap
            s = self.status()
        self.note('advance used', used, 'msg', s['message'], 'pc', s['pc'], 'ev', s['event_running'], 'scene', s['scene'])
        return s

def hero_moved(a, b, minimum=0.1):
    if not a or not b or a.get('hero_position') is None or b.get('hero_position') is None:
        return False
    pa, pb = a['hero_position'], b['hero_position']
    return math.hypot(pb[0] - pa[0], pb[2] - pa[2]) > minimum

def in_field_control(s, scene=None):
    return (s.get('scene') is not None and (scene is None or s['scene'] == scene)
            and not s.get('event_running') and s.get('hero_position') is not None)

def dialogue(self, active, stage, gap, max_ticks, rearm_after):
    """Shared by Ctl.roar and Ctl.event_dialogue (see their docstrings)."""
    used = 0
    last = None
    armed = False
    stalled = 0
    while active() and used < max_ticks:
        o = self.observe()
        if o['battle'] or not o['field']:
            break
        key = (o['message'], o['text_progress'])
        if key != last:
            last, armed, stalled = key, False, 0
        else:
            stalled += 1
            if not armed or stalled >= rearm_after:
                self.step(1, DOWN)
                self.step(10, 0)
                used += 11
                armed, stalled = True, 0
                after = self.observe()
                need(after['choice'] == o['choice'], stage,
                     'a Down moved the dialogue cursor', before=o['choice'], after=after['choice'],
                     frame=after['observe_frame'])
        self.step(4, CONFIRM)
        self.step(gap, 0)
        used += 4 + gap
    return used
