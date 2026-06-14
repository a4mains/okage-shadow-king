"""Drive one prepared run from the unchanged prepared prefix to a natural
Chapter 2 contact using only ordinary journaled controls.

Stages (each raises StageFailure on failure, never returns success unverified):
  route-prefix  prelude and prepared requests finished; hero on the road (scene 1)
  route-walk    ordinary walk reaches the Rosalyn event
  route-rosalyn the pinned historical Rosalyn segment leaves message 20107, no event
  encounter     ordinary patrol reaches a natural contact (native log line)
The Rosalyn segment is read only from RUN/historical-requests.txt (pinned).
"""
import json
import os
import signal
import time
from pathlib import Path

from . import encounter, pins
from .outcome import StageFailure, need, run_stage

DIVERGENCE_SIGNATURE = 'queued scene=73 entrance=2 frame=2394'
PREPARED_COMPLETE = 'DRIVER_PREPARED_REQUESTS_COMPLETE'
ROSALYN_SEGMENT = (98, 293)
WALK = [(-128, 780), (-118, 753), (-164, 741), (-168, 684), (-183, 645), (-178, 660), (-152, 672),
        (-159, 624), (-138, 565), (-161, 571), (-194, 506), (-184, 479), (-187, 461), (-175, 458),
        (-162, 406), (-155, 353), (-142, 318), (-161, 317), (-163, 252), (-158, 241)]

def wait_for(c, pattern, timeout=3600, poll=5.0):
    """Host-side wait for a native log line (no game-time effect)."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pattern in c.log_since(0):
            return True
        if not c.alive():
            return pattern in c.log_since(0)
        time.sleep(poll)
    return False

def rosalyn_segment(run, manifest, prefix_mode='historical'):
    if prefix_mode == 'dev':
        name = pins.DEV_DIR + '/' + pins.DEV_REQUEST_TAPES[0]
        need(name in manifest.get('artifacts', {}), 'route-rosalyn', 'dev Rosalyn tape is not pinned')
        lines = (Path(run) / name).read_text().splitlines()
        count = len(lines)
    else:
        lines = (Path(run) / pins.TAPES[0]).read_text().splitlines()
        count = manifest['historical_prefix_count']
    lo, hi = ROSALYN_SEGMENT
    need(count >= hi and len(lines) >= hi, 'route-rosalyn',
         'pinned historical tape shorter than the Rosalyn segment', length=len(lines))
    seg = [l for l in lines[lo:hi] if l.startswith('step ') or l == 'status']
    need(len(seg) > 0, 'route-rosalyn', 'empty Rosalyn segment')
    return seg

def terminate(run, note):
    proc = json.loads((Path(run) / 'process.json').read_text())
    try:
        os.kill(proc['native_pid'], signal.SIGTERM)
    except OSError:
        pass
    (Path(run) / 'termination-note.json').write_text(json.dumps({'note': note}) + '\n')

def prefix(c, run, manifest, timeout=3600):
    prelude = manifest['prelude_ticks']
    need(wait_for(c, f'Native frames={prelude} ', timeout), 'route-prefix', 'prelude did not finish',
         prelude=prelude)
    if DIVERGENCE_SIGNATURE in c.log_since(0):
        terminate(run, 'prelude diverged: ' + DIVERGENCE_SIGNATURE + '; terminated candidate')
        raise StageFailure('prelude-diverged', 'prelude diverged: ' + DIVERGENCE_SIGNATURE)
    need(wait_for(c, PREPARED_COMPLETE, timeout), 'route-prefix', 'prepared requests did not complete')
    s = c.status()
    c.note('prefix end', s)
    need(s['scene'] == 1 and s['hero_position'] is not None, 'route-prefix', 'not on the road after the prefix', status=s)
    return {'prefix_end': s}

def walk(c):
    s = None
    for x, z in WALK:
        s = c.walk_to(x, z, tol=3, max_ticks=400, stop=lambda s: s['event_running'])
        if s['event_running']:
            break
    need(s is not None and s['event_running'] and s['scene'] == 1, 'route-walk', 'no Rosalyn event', status=s)
    return {'rosalyn_event': s}

def rosalyn(c, run, manifest, prefix_mode='historical'):
    seg = rosalyn_segment(run, manifest, prefix_mode)
    for line in seg:
        c.send(line)
    s = c.status()
    c.note('after rosalyn', s)
    need(not s['event_running'] and s['message'] == 20107 and s['hero_position'] is not None,
         'route-rosalyn', 'Rosalyn state unexpected', status=s)
    return {'segment_requests': len(seg), 'after': s}

STOPS = ('prefix', 'walk', 'rosalyn', 'contact')

PREFIX_MODES = ('historical', 'dev')

def main(c, run, manifest, stop='contact', record=None, prefix_mode='historical'):
    """Run stages up to `stop`. `record(name, evidence)` receives each verified stage.
    prefix_mode 'dev' re-times the retained tape at the actual bedroom activation
    (dev_route.py): a documented deviation, never the unchanged historical prefix."""
    record = record or (lambda name, ev: None)
    if stop not in STOPS:
        raise ValueError('stop must be one of ' + ', '.join(STOPS))
    if prefix_mode not in PREFIX_MODES:
        raise ValueError('prefix_mode must be one of ' + ', '.join(PREFIX_MODES))
    if prefix_mode == 'dev':
        from . import dev_route
        record(dev_route.STAGE, run_stage(dev_route.STAGE, dev_route.prefix, c, run, manifest))
    else:
        record('route-prefix', run_stage('route-prefix', prefix, c, run, manifest))
    if stop == 'prefix':
        return
    record('route-walk', run_stage('route-walk', walk, c))
    if stop == 'walk':
        return
    record('route-rosalyn', run_stage('route-rosalyn', rosalyn, c, run, manifest, prefix_mode))
    if stop == 'rosalyn':
        return
    record('encounter', run_stage('encounter', encounter.main, c, run))
