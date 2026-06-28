"""Stage outcomes, exit codes and result records for native_route entrypoints.

Every stage either completes with verified postconditions or raises
StageFailure. Nothing here converts an unverified sequence into success.
"""
import json
import os
import time
from pathlib import Path

PASS = 0
CODES = {
    'prelude-diverged': 2,
    'pins': 10,
    'run-state': 11,
    'route-prefix': 20,
    'route-walk': 21,
    'route-rosalyn': 22,
    'route-dev-prefix': 23,
    'encounter': 30,
    'battle': 40,
    'navigate': 45,
    'home': 50,
    'home-inside': 60,
    'save': 70,
    'restore-route': 80,
    'restore-check': 90,
    'roar-encounters': 95,
    'usage': 64,
    'internal': 99,
}

class StageFailure(Exception):
    """A stage did not reach its verified postcondition."""

    def __init__(self, stage, reason, **details):
        if stage not in CODES:
            raise ValueError('unknown stage ' + repr(stage))
        super().__init__(f'{stage}: {reason}')
        self.stage = stage
        self.code = CODES[stage]
        self.reason = reason
        self.details = details

    def record(self):
        return {'stage': self.stage, 'code': self.code, 'reason': self.reason,
                'details': _jsonable(self.details)}

def need(ok, stage, reason, **details):
    if not ok:
        raise StageFailure(stage, reason, **details)

def run_stage(stage, fn, *args, **kwargs):
    """Run one stage. A StageFailure raised by a nested stage (for example a
    battle inside the home walk) propagates unchanged; any other exception
    (native exit, protocol error, KeyError on a missing observation) becomes a
    failure of this stage. Never returns a success value for an exception."""
    try:
        result = fn(*args, **kwargs)
    except StageFailure:
        raise
    except KeyboardInterrupt:
        raise
    except BaseException as e:
        raise StageFailure(stage, 'exception: ' + type(e).__name__ + ': ' + str(e)[-2000:]) from e
    if isinstance(result, (bool, int)):
        raise StageFailure(stage, f'stage returned a bare status code {result!r} instead of verified evidence')
    return result

def utc():
    return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())

def _jsonable(value):
    try:
        json.dumps(value)
        return value
    except (TypeError, ValueError):
        if isinstance(value, dict):
            return {str(k): _jsonable(v) for k, v in value.items()}
        if isinstance(value, (list, tuple)):
            return [_jsonable(v) for v in value]
        return repr(value)

def write_json_atomic(path, value):
    path = Path(path)
    tmp = path.with_name('.' + path.name + '.tmp.' + str(os.getpid()))
    tmp.write_text(json.dumps(_jsonable(value), indent=1) + '\n')
    os.replace(tmp, path)

def write_result(path, entrypoint, stages, failure=None, extra=None):
    """Write RUN/<name>.json. status is PASS only when failure is None."""
    record = {'entrypoint': entrypoint, 'utc': utc(),
              'status': 'PASS' if failure is None else 'FAIL',
              'exit_code': PASS if failure is None else failure.code,
              'stages': stages}
    if failure is not None:
        record['failure'] = failure.record()
    if extra:
        record.update(extra)
    write_json_atomic(path, record)
    return record['exit_code']
