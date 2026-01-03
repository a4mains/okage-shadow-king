#!/usr/bin/env python3
"""Execute bounded native headless-control checks with fresh, private save paths.

This checks the supplied executable, not source introspection. No reference VM,
window, captured-state input, or pre-existing save directory is used. Continuous
host loading may progress while stdin is idle; this is not a timing test.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import time

from compare_frame_images import decode_png

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

def check_view(path, populated_lower_half=False):
    data = path.read_bytes()
    require(data.startswith(b'\x89PNG\r\n\x1a\n'), 'Invalid live-view PNG signature')
    require(data[12:16] == b'IHDR', 'Missing PNG IHDR')
    width = int.from_bytes(data[16:20], 'big')
    height = int.from_bytes(data[20:24], 'big')
    require((width, height) == (640, 448), f'Unexpected live-view extent {(width, height)!r}')
    require(not list(path.parent.glob(path.name + '.*')), 'Live view emitted capture-family sidecars')
    if populated_lower_half:
        decoded_width, decoded_height, rgba = decode_png(data)
        require((decoded_width, decoded_height) == (640, 448), 'Decoded view extent changed')
        lower = 224 * decoded_width * 4
        colors = {rgba[lower + y * decoded_width * 4 + x * 4:
                       lower + y * decoded_width * 4 + x * 4 + 3]
                  for y in range(0, 224, 3) for x in range(0, 640, 3)}
        require(len(colors) >= 8, 'Live field view lower half is not populated')

def read_status_rows(log_path):
    rows = []
    for line in log_path.read_text(errors='replace').splitlines():
        if line.startswith('{'):
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            if 'hero_position' in row:
                rows.append(row)
    require(len(rows) == 2, f'Expected two status JSON responses, got {len(rows)}')
    for row in rows:
        require(set(row) == {'frame', 'scene', 'entrance', 'event_running', 'pc', 'message',
                             'choice', 'camera_yaw', 'hero_position', 'guard_pending_opcode'},
                'Status JSON fields differ from the lightweight schema')
    return rows

def capture_family_hashes(path):
    prefix = path.name
    return {item.name[len(prefix):]: digest(item)
            for item in path.parent.glob(prefix + '*') if item.is_file()}

def run_field_case(args, output, native, game_data):
    source_save = args.field_save.resolve(strict=True)
    source_buttons = args.field_buttons.resolve(strict=True)
    source_directions = args.field_directions.resolve(strict=True)
    require(source_save.is_file() and source_buttons.is_file() and source_directions.is_file(),
            'Field verification inputs must be regular files')
    field_case = output / 'field-observation'
    field_case.mkdir()
    saves = field_case / 'saves'
    saves.mkdir()
    save_copy = saves / 'bkmo0.dat'
    shutil.copy2(source_save, save_copy)
    buttons_copy = field_case / 'buttons.txt'
    directions_copy = field_case / 'directions.txt'
    shutil.copy2(source_buttons, buttons_copy)
    shutil.copy2(source_directions, directions_copy)
    control = field_case / 'control'
    commands = 'status\nview field-before\nstep 20 1000\nstatus\nview field-after\nquit\n'
    command = [str(native), '--game-data', str(game_data), '--save-directory', str(saves),
               '--frames', '2800', '--buttons', str(buttons_copy), '--directions', str(directions_copy),
               '--headless-control', str(control)]
    started = time.monotonic()
    with (field_case / 'run.log').open('wb') as log:
        run = subprocess.run(command, input=commands.encode(), stdout=log, stderr=subprocess.STDOUT,
                             cwd=field_case, timeout=300)
    log_text = (field_case / 'run.log').read_text(errors='replace')
    require(run.returncode == 0, f'Field observation native exit={run.returncode}: {log_text[-2000:]}')
    statuses = read_status_rows(field_case / 'run.log')
    require([row['frame'] for row in statuses] == [2800, 2820], 'Field status frame changed unexpectedly')
    for row in statuses:
        require(row['scene'] == 74 and row['entrance'] == 1,
                'Native save did not restore to the retained ordinary bedroom field entry')
        require(row['hero_position'] is not None and len(row['hero_position']) == 3 and
                all(isinstance(value, (int, float)) and math.isfinite(value)
                    for value in row['hero_position']), 'Field status omitted HERO position')
        require(isinstance(row['camera_yaw'], (int, float)) and math.isfinite(row['camera_yaw']),
                'Field status omitted camera yaw')
    displacement = math.sqrt(sum((a - b) ** 2 for a, b in zip(statuses[0]['hero_position'],
                                                                statuses[1]['hero_position'])))
    require(displacement > 1e-5, 'Ordinary field movement did not change the reported HERO position')
    check_view(control / 'field-before.view.png', populated_lower_half=True)
    check_view(control / 'field-after.view.png', populated_lower_half=True)
    rows = journal(control / 'inputs.jsonl')
    attempts = [row['frame'] for row in rows if row.get('kind') == 'tick_attempt']
    completed = [row['frame'] for row in rows if row.get('kind') == 'tick_complete']
    require(attempts == list(range(1, 2821)) and completed == attempts,
            'Field status/view commands admitted or omitted simulation ticks')
    return {
        'passed': True, 'command': command, 'input': commands,
        'save_source': str(source_save), 'save_source_sha256': digest(source_save),
        'save_copy_sha256': digest(save_copy),
        'buttons_sha256': digest(buttons_copy), 'directions_sha256': digest(directions_copy),
        'statuses': statuses, 'hero_displacement': displacement,
        'view_extents': [[640, 448], [640, 448]], 'lower_half_populated': True,
        'tick_attempts': len(attempts), 'tick_completions': len(completed),
        'exit_code': run.returncode, 'elapsed_seconds': time.monotonic() - started,
    }

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--game-data', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--field-save', type=Path,
                        help='Optional genuine native bkmo0.dat for a Restore-to-field observation check')
    parser.add_argument('--field-buttons', type=Path,
                        help='Retained ordinary Restore input tape; required with --field-save')
    parser.add_argument('--field-directions', type=Path,
                        help='Retained ordinary Restore direction tape; required with --field-save')
    args = parser.parse_args()
    native = args.native.resolve(strict=True)
    if not args.game_data.is_absolute():
        parser.error('--game-data must be absolute')
    game_data = args.game_data.resolve(strict=True)
    if not native.is_file() or not game_data.is_dir():
        parser.error('Native executable and game-data directory are required')
    field_inputs = (args.field_save, args.field_buttons, args.field_directions)
    if any(field_inputs) and not all(field_inputs):
        parser.error('--field-save, --field-buttons and --field-directions must be supplied together')
    if args.output.exists() or args.output.is_symlink():
        parser.error('--output must be a fresh directory (including no symlink)')
    output = args.output.absolute()
    output.mkdir()
    output = output.resolve(strict=True)
    report = {'schema': 'okage-headless-control-verification-v1',
              'native': str(native), 'native_sha256': digest(native),
              'game_data': str(game_data), 'tool_sha256': digest(Path(__file__)),
              'scope': 'Executed short cold startup and retained Session control; no gameplay, render fidelity, or loader timing equivalence',
              'cases': []}
    prelude = [(1, 64, 64)]
    cases = [
        ('observations', 'status\nview panel\ncapture before\nstatus\nview panel2\ncapture after\nquit\n',
         prelude, None),
        ('eof_edges', 'capture a\ncapture b\nstep 2 0040\nstep 1 0000\nstep 1 0040\n',
         prelude + [(2, 64, 0), (3, 64, 0), (4, 0, 0), (5, 64, 64)], None),
        ('quit', 'quit\nstep 1 0040\n', prelude, None),
        ('extra_token', 'step 1 0040 extra\n', prelude, 'Expected:'),
        ('zero_ticks', 'step 0 0040\n', prelude, 'Step requires'),
        ('excess_ticks', 'step 6001 0040\n', prelude, 'Step requires'),
        ('overflow_ticks', 'step 4294967296 0040\n', prelude, 'Step requires'),
        ('overflow_mask', 'step 1 10000\n', prelude, 'Step requires'),
        ('duplicate_capture', 'capture a\ncapture a\n', prelude, 'fresh output family'),
        ('duplicate_view', 'view panel\nview panel\n', prelude, 'reused headless observation name'),
        ('cross_kind_name', 'view shared\ncapture shared\n', prelude, 'reused headless observation name'),
        ('reserved_view', 'view FINAL\n', prelude, 'reserved view name'),
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
                if name == 'observations':
                    statuses = read_status_rows(case / 'run.log')
                    require([row['frame'] for row in statuses] == [1, 1],
                            'Status observation advanced or misreported frame')
                    require(statuses[0] == statuses[1],
                            'Status observations changed between read-only commands')
                    check_view(control / 'panel.view.png')
                    check_view(control / 'panel2.view.png')
                    before, before_vars = check_capture(control / 'before.png', 1)
                    after, after_vars = check_capture(control / 'after.png', 1)
                    require(before == after and before_vars == after_vars,
                            'Read-only observations changed captured Session state')
                    require(capture_family_hashes(control / 'before.png') ==
                            capture_family_hashes(control / 'after.png'),
                            'Read-only observations changed capture, trace or audio bytes')
                    kinds = [row['kind'] for row in journal(control / 'inputs.jsonl')]
                    require(kinds.count('tick_attempt') == 1 and kinds.count('tick_complete') == 1,
                            'Status/view admitted an input-journal tick')
                if name == 'duplicate_view':
                    check_view(control / 'panel.view.png')
                if name == 'cross_kind_name':
                    check_view(control / 'shared.view.png')
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
    if args.field_save:
        try:
            report['field_case'] = run_field_case(args, output, native, game_data)
            print('field_observation: PASS', flush=True)
        except Exception as exc:
            report['field_case'] = {'passed': False, 'error': f'{type(exc).__name__}: {exc}'}
            report['passed'] = False
            print('field_observation: FAIL', flush=True)
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    report['native_sha256_after'] = digest(native)
    report['passed'] = report['passed'] and report['native_sha256_after'] == report['native_sha256']
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(output / 'report.json')
    return 0 if report['passed'] else 1

if __name__ == '__main__':
    sys.exit(main())
