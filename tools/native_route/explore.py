#!/usr/bin/env python3
"""Interactive ordinary-input exploration of a live prepared run, for an
operator mapping an unfamiliar route segment (routing policy: Sonnet for
navigation through unfamiliar states). It sends only pad steps and read-only
observations through the run's headless control, like every other stage.

Every subcommand appends {utc, argv, result} to RUN/explore-journal.jsonl, so
the exploration is reproducible from the journal and the native input
journal. This is rehearsal tooling: an accepted route uses a pinned stage
command whose waypoints came from such a trail.

  explore.py RUN status
  explore.py RUN view NAME
  explore.py RUN walk X Z [--tol 3] [--max-ticks 600]   # fights any battle met
  explore.py RUN step TICKS DIR                          # DIR: up/down/left/right/ul/ur/dl/dr (screen)
  explore.py RUN confirm                                 # one Confirm press, then 30 idle ticks
  explore.py RUN advance [--max-ticks 4000]              # Confirm until no event runs
  explore.py RUN fight                                   # fight an active battle to victory
  explore.py RUN trail                                   # hero positions from this journal
"""
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from native_route import battle, pilot  # noqa: E402

DIRS = {'up': pilot.UP, 'down': pilot.DOWN, 'left': pilot.LEFT, 'right': pilot.RIGHT,
        'ul': pilot.UP | pilot.LEFT, 'ur': pilot.UP | pilot.RIGHT,
        'dl': pilot.DOWN | pilot.LEFT, 'dr': pilot.DOWN | pilot.RIGHT}

def journal(run, argv, result):
    with open(Path(run) / 'explore-journal.jsonl', 'a') as f:
        f.write(json.dumps({'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'argv': argv,
                            'result': result}) + '\n')

def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('status')
    sub.add_parser('confirm')
    sub.add_parser('fight')
    sub.add_parser('trail')
    p = sub.add_parser('view')
    p.add_argument('name')
    p = sub.add_parser('walk')
    p.add_argument('x', type=float)
    p.add_argument('z', type=float)
    p.add_argument('--tol', type=float, default=3.0)
    p.add_argument('--max-ticks', type=int, default=600)
    p = sub.add_parser('step')
    p.add_argument('ticks', type=int)
    p.add_argument('dir', choices=sorted(DIRS))
    p = sub.add_parser('advance')
    p.add_argument('--max-ticks', type=int, default=4000)
    a = ap.parse_args(argv)
    run = Path(a.run)
    if a.cmd == 'trail':
        rows = [json.loads(l) for l in (run / 'explore-journal.jsonl').read_text().splitlines() if l.strip()]
        pts = [r['result']['status']['hero_position'] for r in rows
               if isinstance(r.get('result'), dict) and r['result'].get('status', {}).get('hero_position')]
        print(json.dumps([[round(p[0], 1), round(p[2], 1)] for p in pts]))
        return 0
    c = pilot.Ctl(run, verbose=False)
    result = {}
    if a.cmd == 'view':
        c.send(f'view {a.name}')
        result['view'] = str(run / 'control' / f'{a.name}.view.png')
    elif a.cmd == 'walk':
        c.walk_to(a.x, a.z, tol=a.tol, max_ticks=a.max_ticks, stop=lambda s: s['event_running'])
        result['battle'] = battle.fight_if_needed(c, run) is not None
    elif a.cmd == 'step':
        c.step(a.ticks, DIRS[a.dir])
        c.step(1, 0)
    elif a.cmd == 'confirm':
        c.step(2, 0)
        c.step(1, pilot.CONFIRM)
        c.step(30, 0)
    elif a.cmd == 'advance':
        c.advance(lambda s: not s['event_running'], confirm_gap=40, max_ticks=a.max_ticks)
    elif a.cmd == 'fight':
        result['battle'] = battle.fight_if_needed(c, run) is not None
    result['status'] = c.status()
    journal(run, argv, result)
    print(json.dumps(result))
    return 0

if __name__ == '__main__':
    sys.exit(main())
