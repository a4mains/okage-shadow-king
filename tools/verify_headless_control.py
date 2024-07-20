#!/usr/bin/env python3
"""Execute bounded native headless-control checks with fresh, private save paths.

This checks the supplied executable, not source introspection. No reference VM,
window, captured-state input, or pre-existing save directory is used. Continuous
host loading may progress while stdin is idle; this is not a timing test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def require(condition, message):
    if not condition:
        raise AssertionError(message)

def journal(path):
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    require(rows and rows[0].get('schema') == 'okage-native-input-journal-v1',
            'Missing input-journal schema')
    return rows

def check_journal(rows, expected, terminal):
    attempts = [(r['frame'], r['held'], r['pressed']) for r in rows
                if r.get('kind') == 'tick_attempt']
    completed = [r['frame'] for r in rows if r.get('kind') == 'tick_complete']
    require(attempts == expected, f'Input attempts differ: {attempts!r}')
    require(completed == [r[0] for r in expected], 'Missing/extra completed ticks')
    ends = [r for r in rows if r.get('kind') in ('finish', 'error')]
    require(len(ends) == 1 and ends[0]['kind'] == terminal and rows[-1] == ends[0],
            f'Expected exactly one terminal {terminal}')
    require(ends[0]['frame'] == expected[-1][0], 'Incorrect terminal frame')
    active, pending = True, None
    for row in rows[1:]:
        kind = row['kind']
        if kind == 'tick_attempt':
            require(active and pending is None, 'Tick attempted while paused/pending')
            pending = row['frame']
        elif kind == 'tick_complete':
            require(active and pending == row['frame'], 'Unpaired completion')
            pending = None
        elif kind == 'pause':
            require(active and pending is None, 'Invalid pause boundary')
            active = False
        elif kind == 'resume':
            require(not active and pending is None, 'Invalid resume boundary')
            active = True
        elif kind in ('finish', 'error'):
            require(pending is None, 'Unexpected incomplete tick')
        else:
            raise AssertionError(f'Unexpected journal event {kind!r}')

def check_capture(path, frame):
    require(path.read_bytes().startswith(b'\x89PNG\r\n\x1a\n'), 'Invalid PNG signature')
    state = json.loads(Path(str(path) + '.json').read_text())
    require(state['frames'] == frame, f'{path.name}: unexpected frame')
    variables = Path(str(path) + '.variables.bin').read_bytes()
    require(len(variables) >= 0xd78, 'Truncated variables capture')
    return state, variables

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--game-data', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve(strict=True)
    if not args.game_data.is_absolute():
        parser.error('--game-data must be absolute')
    game_data = args.game_data.resolve(strict=True)
    if not native.is_file() or not game_data.is_dir():
        parser.error('Native executable and game-data directory are required')
    if args.output.exists() or args.output.is_symlink():
        parser.error('--output must be a fresh directory (including no symlink)')
    output = args.output.absolute()
    output.mkdir()
    output = output.resolve(strict=True)
    report = {'schema': 'okage-headless-control-verification-v1',
              'native': str(native), 'native_sha256': digest(native),
              'game_data': str(game_data), 'tool_sha256': digest(Path(__file__)),
              'scope': "Executed short cold startup and retained Session control; no gameplay, render fidelity, or loader timing equivalence",
              'cases': []}
    prelude = [(1, 64, 64)]
    cases = [
        ('eof_edges', 'capture a\ncapture b\nstep 2 0040\nstep 1 0000\nstep 1 0040\n',
         prelude + [(2, 64, 0), (3, 64, 0), (4, 0, 0), (5, 64, 64)], None),
        ('quit', 'quit\nstep 1 0040\n', prelude, None),
        ('extra_token', 'step 1 0040 extra\n', prelude, 'Expected:'),
        ('zero_ticks', 'step 0 0040\n', prelude, 'Step requires'),
        ('excess_ticks', 'step 6001 0040\n', prelude, 'Step requires'),
        ('overflow_ticks', 'step 4294967296 0040\n', prelude, 'Step requires'),
        ('overflow_mask', 'step 1 10000\n', prelude, 'Step requires'),
        ('duplicate_capture', 'capture a\ncapture a\n', prelude, 'fresh output family'),
        ('reserved_case', 'capture FINAL\n', prelude, 'reserved capture name'),
        ('existing_directory', '', [], 'fresh directory'),
    ]
    for name, commands, expected, error_text in cases:
        case = output / name
        case.mkdir()
        control, saves = case / 'control', case / 'saves'
        if name == 'existing_directory':
            control.mkdir()
            (control / 'sentinel').write_bytes(b'must remain unchanged\n')
        (case / 'stdin.txt').write_text(commands)
        command = [str(native), '--game-data', str(game_data), '--save-directory', str(saves),
                   '--frames', '1', '--press', '1', '--capture', str(case / 'prelude.png'),
                   '--headless-control', str(control)]
        result = {'name': name, 'command': command, 'cwd': str(case),
                  'stdin': str(case / 'stdin.txt'), 'expected_attempts': expected,
                  'save_directory': str(saves)}
        started = time.monotonic()
        try:
            with (case / 'run.log').open('wb') as log:
                run = subprocess.run(command, input=commands.encode(), stdout=log,
                                     stderr=subprocess.STDOUT, cwd=case, timeout=180)
            result['exit_code'] = run.returncode
            log_text = (case / 'run.log').read_text(errors='replace')
            require(run.returncode == (1 if error_text else 0), 'Unexpected process exit status')
            if error_text:
                require(error_text in log_text, f'Failure was not expected rejection {error_text!r}')
            if name == 'existing_directory':
                require((control / 'sentinel').read_bytes() == b'must remain unchanged\n',
                        'Existing directory modified')
                require(sorted(p.name for p in control.iterdir()) == ['sentinel'],
                        'Existing directory gained output')
                require(not (case / 'prelude.png').exists(), 'Rejection happened after replay capture')
            else:
                rows = journal(control / 'inputs.jsonl')
                check_journal(rows, expected, 'error' if error_text else 'finish')
                result['journal_rows'] = len(rows)
                check_capture(case / 'prelude.png', 1)
                if not error_text:
                    check_capture(control / 'final.png', expected[-1][0])
                else:
                    require(not (control / 'final.png').exists(), 'Failure produced successful final capture')
                if name == 'eof_edges':
                    a, av = check_capture(control / 'a.png', 1)
                    b, bv = check_capture(control / 'b.png', 1)
                    require(av == bv, 'Capture-only commands mutated variables')
                    require(a == b, 'Capture-only commands mutated scalar state')
                    require(digest(control / 'a.png') == digest(control / 'b.png'),
                            'Repeated same-frame captures changed image')
                if name == 'duplicate_capture':
                    check_capture(control / 'a.png', 1)
            require(not saves.exists() or not any(saves.rglob('*')),
                    'Short cold control unexpectedly wrote save data')
            result['passed'] = True
        except Exception as exc:
            result['passed'] = False
            result['error'] = f'{type(exc).__name__}: {exc}'
        result['elapsed_seconds'] = time.monotonic() - started
        result['files'] = {str(p.relative_to(case)): {'sha256': digest(p), 'bytes': p.stat().st_size}
                           for p in sorted(case.rglob('*')) if p.is_file()}
        report['cases'].append(result)
        report['passed'] = all(c['passed'] for c in report['cases'])
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f"{name}: {'PASS' if result['passed'] else 'FAIL'}", flush=True)
    report['native_sha256_after'] = digest(native)
    report['passed'] = report['passed'] and report['native_sha256_after'] == report['native_sha256']
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(output / 'report.json')
    return 0 if report['passed'] else 1

if __name__ == '__main__':
    sys.exit(main())
