"""Prepare + detached launch, and the explicit prelude-candidate framework.

The historical prelude is host-load dependent: some fresh processes queue the
bedroom exit at frame 2394 (route.DIVERGENCE_SIGNATURE). Candidate selection
keeps that explicit: every candidate run is retained, every rejection reason is
recorded in PARENT/PREFIX-candidates.json together with the fixed rule below,
and nothing is deleted.
"""
import json
import string
import subprocess
import sys
import time
from pathlib import Path

from . import outcome, pins, session_driver

SELECTION_RULE = (
    'Candidates PREFIXa, PREFIXb, ... (at most MAX) are prepared fresh with identical pinned inputs and '
    'launched one at a time; each runs the pinned strict-full route. A candidate is rejected only when '
    'strict-full exits 2 AND its route-result.json names stage prelude-diverged (native log contains '
    '"queued scene=73 entrance=2 frame=2394"; its native process was terminated). Any other outcome ends '
    'the search: exit 0 with route-result status PASS selects that candidate; any other exit, a missing or '
    'inconsistent route-result.json, or a launch refusal ends the search as failed. Every candidate '
    'directory, log and result is retained; none is deleted or excluded from this record.')

def cli_path(run):
    return Path(run) / pins.TOOLS_REL / 'cli.py'

def pinned_command(run, *args):
    return [sys.executable, '-I', '-B', str(cli_path(run))] + [str(a) for a in args]

def launch_detached(run, ready_timeout=600):
    """Start the pinned driver (RUN/tools/native_route/cli.py launch RUN) in its
    own session and return process.json once the driver published it."""
    run = Path(run).resolve()
    with (run / 'driver.out').open('x') as out:
        p = subprocess.Popen(pinned_command(run, 'launch', run), stdin=subprocess.DEVNULL, stdout=out,
                             stderr=subprocess.STDOUT, cwd=str(run), start_new_session=True)
    t0 = time.time()
    while not (run / 'process.json').is_file():
        if p.poll() is not None:
            raise pins.PinError(['driver exited before launch (code %s): %s'
                                 % (p.returncode, (run / 'driver.out').read_text()[-2000:])])
        if time.time() - t0 > ready_timeout:
            raise pins.PinError(['driver did not publish process.json'])
        time.sleep(0.1)
    return json.loads((run / 'process.json').read_text())

def prepare_and_launch(run, **prepare_kwargs):
    session_driver.prepare(run, **prepare_kwargs)
    return launch_detached(run)

def classify(exit_code, result):
    """Return (disposition, continue_search)."""
    failure = (result or {}).get('failure') or {}
    if exit_code == outcome.CODES['prelude-diverged'] and failure.get('stage') == 'prelude-diverged':
        return 'rejected: prelude-diverged (' + failure.get('reason', '') + ')', True
    if result is None:
        return f'search-failed: strict-full exit {exit_code} without route-result.json', False
    if result.get('exit_code') != exit_code:
        return f'search-failed: exit {exit_code} disagrees with route-result exit {result.get("exit_code")}', False
    if exit_code == 0 and result.get('status') == 'PASS' and not failure:
        return 'selected', False
    return f'search-failed: stage {failure.get("stage")} code {failure.get("code")}: {failure.get("reason")}', False

def run_candidates(parent, prefix, max_candidates, target_save, prepare_kwargs, strict_args=()):
    parent = Path(parent).resolve()
    if not 1 <= max_candidates <= 26:
        raise ValueError('max candidates must be 1..26')
    record_path = parent / f'{prefix}-candidates.json'
    if record_path.exists():
        raise pins.PinError([f'refusing existing candidate record {record_path}'])
    record = {'prefix': prefix, 'parent': str(parent), 'max_candidates': max_candidates,
              'selection_rule': SELECTION_RULE, 'target_save': target_save,
              'prepare': {k: (str(v) if isinstance(v, Path) else v) for k, v in prepare_kwargs.items()},
              'started_utc': outcome.utc(), 'candidates': [], 'selected': None, 'outcome': 'running'}
    outcome.write_json_atomic(record_path, record)
    for letter in string.ascii_lowercase[:max_candidates]:
        name = prefix + letter
        run = parent / name
        entry = {'name': name, 'run': str(run), 'started_utc': outcome.utc()}
        record['candidates'].append(entry)
        try:
            entry['process'] = prepare_and_launch(run, label=name, **prepare_kwargs)
        except (pins.PinError, OSError) as e:
            entry['disposition'] = 'search-failed: not launched: ' + str(e)
            record['outcome'] = 'failed'
            break
        with (run / 'strict-full.out').open('x') as out:
            code = subprocess.run(pinned_command(run, 'strict-full', run, '--target-save', target_save, *strict_args),
                                  stdout=out, stderr=subprocess.STDOUT, cwd=str(run)).returncode
        try:
            result = json.loads((run / 'route-result.json').read_text())
        except (OSError, ValueError):
            result = None
        entry['exit_code'] = code
        entry['route_result'] = result and {k: result.get(k) for k in ('status', 'exit_code', 'failure')}
        entry['disposition'], again = classify(code, result)
        entry['finished_utc'] = outcome.utc()
        outcome.write_json_atomic(record_path, record)
        if not again:
            record['outcome'] = 'selected' if entry['disposition'] == 'selected' else 'failed'
            if record['outcome'] == 'selected':
                record['selected'] = name
            break
    else:
        record['outcome'] = 'exhausted: every candidate diverged'
    record['finished_utc'] = outcome.utc()
    outcome.write_json_atomic(record_path, record)
    return 0 if record['selected'] else 1
