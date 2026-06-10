"""DEVELOPMENT prefix (prefix_mode "dev"), ported from the frozen
claude-tools/dev_route.py. It is NOT the strict historical prefix.

The run is prepared with an 808-tick prelude (the title) from a prepared
directory whose historical/extension request tapes are empty. The retained
prelude tape (full-prelude-buttons/directions) is then replayed as ordinary
`step` requests: tape frames 808..2591 are replayed until the bedroom activation
(status scene 74) at actual frame A, then tape frames 2591..21644 are replayed
aligned at A (the held mask at 2591 continues), then the retained
prefix-historical and prefix-extension requests are sent. The retained tape is
therefore re-timed at the actual bedroom activation, so this is a documented
deviation from the unchanged historical prefix.

Every input is read from the RUN's pinned copies under RUN/dev/. Only ordinary
`step TICKS MASK` and `status` requests are sent; decisions use status only.
"""
from pathlib import Path

from . import pins
from .outcome import need

STAGE = 'route-dev-prefix'
PRELUDE = 808
ACTIVATION_SCENE = 74
ROAD_SCENE = 1
TAPE_ALIGN = 2591
TAPE_END = 21644
POLL = 20

def pinned_problems(run, manifest):
    """Reasons the dev route may not run on this RUN (empty when it may)."""
    problems = []
    if list(manifest.get('dev_tapes') or []) != list(pins.DEV_TAPES):
        problems.append('dev tapes are not pinned in the manifest (prepare from a directory holding '
                        + ', '.join(pins.DEV_TAPES) + ')')
        return problems
    artifacts = manifest.get('artifacts') or {}
    for name in pins.DEV_TAPES:
        rel = pins.DEV_DIR + '/' + name
        p = Path(run) / rel
        if rel not in artifacts:
            problems.append('manifest does not pin ' + rel)
        elif not p.is_file() or p.is_symlink() or pins.sha256(p) != artifacts[rel]:
            problems.append('missing or changed pinned dev tape ' + rel)
    return problems

def load_tape(run):
    """(mask_at(frame), frames) from the pinned prelude button/direction tapes."""
    marks = [pins.read_prelude_tape(Path(run) / pins.DEV_DIR / n) for n in pins.DEV_PRELUDE_TAPES]
    frames = sorted({f for m in marks for f, _ in m})

    def held_at(tape, frame):
        h = 0
        for f, m in tape:
            if f <= frame:
                h = m
            else:
                break
        return h
    return (lambda frame: held_at(marks[0], frame) | held_at(marks[1], frame)), frames

def replay(c, mask_at, frames, start, end, until=None):
    """Replay tape frames [start, end) as ordinary steps. With `until`, steps
    are at most POLL ticks and `until()` (a status poll) runs after each one.
    Returns the tape frame reached."""
    cuts = [f for f in frames if start < f < end] + [end]
    f = start
    for nxt in cuts:
        m = mask_at(f)
        n = nxt - f
        while n > 0:
            k = min(n, 6000) if until is None else min(n, POLL)
            c.step(k, m)
            n -= k
            f += k
            if until and until():
                return f
    return f

def retained_requests(run):
    out = []
    for name in pins.DEV_REQUEST_TAPES:
        for line in pins.read_tape(Path(run) / pins.DEV_DIR / name):
            if line.startswith('step ') or line == 'status':
                out.append(line)
    return out

def prefix(c, run, manifest, timeout=3600):
    from . import route  # route imports this module lazily too
    problems = pinned_problems(run, manifest)
    need(not problems, 'pins', 'dev route refused: ' + '; '.join(problems))
    need(route.wait_for(c, f'Native frames={PRELUDE} ', timeout), STAGE, 'prelude did not finish', prelude=PRELUDE)
    need(route.wait_for(c, route.PREPARED_COMPLETE, timeout), STAGE, 'prepared requests did not complete')
    s = c.status()
    need(s['frame'] == PRELUDE, STAGE, f'status frame is not {PRELUDE}', status=s)
    mask_at, frames = load_tape(run)

    def active():
        return c.status()['scene'] == ACTIVATION_SCENE
    reached = replay(c, mask_at, frames, PRELUDE, TAPE_ALIGN, until=active)
    while not active():
        c.step(1, 0)
        reached += 1
    activation = c.status()['frame']
    c.note('bedroom activation', activation, 'tape frame reached', reached)
    replay(c, mask_at, frames, TAPE_ALIGN, TAPE_END)
    s = c.status()
    c.note('prelude-equivalent end', s)
    seg = retained_requests(run)
    for line in seg:
        c.send(line)
    s = c.status()
    c.note('after retained requests', s)
    need(s['scene'] == ROAD_SCENE, STAGE, 'not on the road after the retained requests', status=s)
    return {'prefix_mode': 'dev', 'activation_frame': activation, 'tape_frame_reached_at_activation': reached,
            'alignment_tape_frame': TAPE_ALIGN, 'alignment_offset': activation - TAPE_ALIGN,
            'retained_requests': len(seg), 'prefix_end': s}
