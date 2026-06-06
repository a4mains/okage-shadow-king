"""Ordinary-controller native replay with an append-only command file.

prepare: copy the binary, prepared request tapes, prelude button/direction
tapes, genuine saves and this whole tool package into a FRESH run directory and
pin every hash, the game-data stage identity and the Python version.

launch: verify every pin, run the native headless session, replay the prepared
requests unchanged, then forward lines appended to RUN/commands.in. Every
forwarded request is journaled with its origin. Only ordinary pad steps and
read-only observation commands (plus quit) are forwarded; anything else is
rejected and journaled as rejected. No state, seed, timing or outcome is
injected.

The origin label of a prepared request is derived from the pinned manifest
field historical_prefix_count, which prepare computes from the actual length of
the pinned historical tape (294 for navigation-pulse-historical-release-v1).
There is no default: the old --historical-count 294 default could silently
mislabel a different tape (552 prepared requests = 294 historical + 258
retained extension).
"""
import json
import os
import queue
import shutil
import subprocess
import threading
import time
from pathlib import Path

from . import outcome, pins

def native_command(run, manifest):
    r = str(run)
    prelude = str(manifest['prelude_ticks'])
    cmd = [r + '/okage-native', '--game-data', manifest['game_data_stage']['resolved_path'],
           '--save-directory', r + '/saves', '--frames', prelude,
           '--buttons', r + '/inputs/buttons.txt', '--directions', r + '/inputs/directions.txt',
           '--capture', r + '/replay.png', '--capture-frames', prelude,
           '--input-journal', r + '/inputs.jsonl', '--input-log', r + '/reads.jsonl',
           '--battle-live', r + '/battle-live.jsonl', '--headless-control', r + '/control']
    if manifest['newgame_report']:
        cmd[cmd.index('--battle-live'):cmd.index('--battle-live')] = ['--newgame-report', r + '/report']
    return cmd

def prepare(run, binary, stage, prepared, saves, prelude_ticks, newgame_report=False,
            min_free_gib=200, source_repo=None, label=None):
    run = Path(run).resolve()
    if run.exists() or run.is_symlink():
        raise pins.PinError(['refusing existing run directory ' + str(run)])
    if not run.parent.is_dir():
        raise pins.PinError(['run parent does not exist: ' + str(run.parent)])
    free = shutil.disk_usage(run.parent).free
    if free < min_free_gib * 2 ** 30:
        raise pins.PinError([f'refusing: {free / 2 ** 30:.0f} GiB free < {min_free_gib} GiB'])
    if prelude_ticks < 1:
        raise pins.PinError(['prelude ticks must be positive'])
    src = Path(prepared).resolve()
    binary = Path(binary).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise pins.PinError([f'binary {binary} is not an executable file'])
    tapes = {name: pins.read_tape(src / name) for name in pins.TAPES}
    prelude = {name: pins.read_prelude_tape(src / name) for name in pins.PRELUDE_INPUTS}
    save_src = Path(saves).resolve()
    save_files = sorted(save_src.iterdir())
    for p in save_files:
        if p.is_symlink() or not p.is_file():
            raise pins.PinError([f'save source entry {p} is not a regular file'])
    stage_ident = pins.stage_identity(stage)
    tools_src = pins.package_dir()
    tool_names = pins.package_files(tools_src, source=True)
    source_hashes = {name: pins.sha256(tools_src / name) for name in tool_names}

    run.mkdir()
    (run / 'inputs').mkdir()
    (run / 'saves').mkdir()
    (run / pins.TOOLS_REL).mkdir(parents=True)
    artifacts = {}
    for name in pins.TAPES + pins.PRELUDE_INPUTS:
        shutil.copyfile(src / name, run / name)
        artifacts[name] = pins.sha256(run / name)
    shutil.copy2(binary, run / 'okage-native')
    artifacts['okage-native'] = pins.sha256(run / 'okage-native')
    for name in tool_names:
        shutil.copyfile(tools_src / name, run / pins.TOOLS_REL / name)
        artifacts[pins.TOOLS_REL + '/' + name] = pins.sha256(run / pins.TOOLS_REL / name)
        if artifacts[pins.TOOLS_REL + '/' + name] != source_hashes[name]:
            raise pins.PinError([f'tool {name} changed while it was copied'])
    initial_saves = {}
    for p in save_files:
        shutil.copy2(p, run / 'saves' / p.name)
        initial_saves[p.name] = {'sha256': pins.sha256(run / 'saves' / p.name),
                                 'size': (run / 'saves' / p.name).stat().st_size}
    source = None
    if source_repo:
        source = _source_identity(Path(source_repo), run)
        if source.get('diff_file'):
            artifacts['source.diff'] = pins.sha256(run / 'source.diff')
    requests = tapes[pins.TAPES[0]] + tapes[pins.TAPES[1]]
    m = {'schema': 'native-route-run-v1', 'state': 'PREPARED_NOT_LAUNCHED', 'label': label,
         'run': str(run), 'cwd': str(run), 'prepared_from': str(src),
         'source_binary': str(binary), 'source': source, 'artifacts': artifacts,
         'initial_saves': initial_saves, 'saves_from': str(save_src),
         'prelude_ticks': prelude_ticks,
         'historical_prefix_count': len(tapes[pins.TAPES[0]]),
         'extension_count': len(tapes[pins.TAPES[1]]),
         'prepared_requests': len(requests),
         'prepared_endpoint': prelude_ticks + pins.step_ticks(requests),
         'prelude_tape_entries': {k: len(v) for k, v in prelude.items()},
         'game_data_stage': stage_ident, 'python': pins.python_identity(),
         'newgame_report': bool(newgame_report), 'min_free_gib': min_free_gib,
         'policy': 'prepared requests unchanged; later lines only via commands.in, journaled; '
                   'only pad steps and read-only observations; no state/seed/timing/outcome injection'}
    m['native_command'] = native_command(run, m)
    pins.manifest_path(run).write_text(json.dumps(m, indent=1) + '\n')
    (run / 'commands.in').touch()
    return m

def _source_identity(repo, run):
    """Read-only git identity of the checkout that built the binary."""
    def git(*args):
        return subprocess.run(['git', '-C', str(repo)] + list(args), capture_output=True, text=True, check=True).stdout
    head = git('rev-parse', 'HEAD').strip()
    diff = git('diff', 'HEAD')
    (run / 'source.diff').write_text(diff)
    return {'repo': str(repo.resolve()), 'head': head, 'dirty': bool(diff), 'diff_file': 'source.diff'}

def launch(run):
    """Run the native process to completion. Must be called through the pinned
    RUN/tools copy (cli.py enforces that) after verify_run(run, 'launch')."""
    run = Path(run).resolve()
    m = pins.verify_run(run, 'launch', 'launch')
    if m['native_command'] != native_command(run, m):
        raise pins.PinError(['manifest native_command does not match the pinned inputs'])
    tapes = [pins.read_tape(run / name) for name in pins.TAPES]
    historical_count = m['historical_prefix_count']
    if historical_count != len(tapes[0]):
        raise pins.PinError(['historical_prefix_count mismatch'])
    requests = tapes[0] + tapes[1]
    names = {s.split()[1] for s in requests if s.split()[0] in ('capture', 'view')}
    events = queue.Queue()
    manifest_sha = pins.sha256(pins.manifest_path(run))
    state = {'acknowledged': 0, 'consumed': 0, 'rejected': 0, 'alive': True, 'prepared_complete': False}

    def publish():
        outcome.write_json_atomic(run / 'driver-state.json', state)

    with (run / 'stdout.log').open('x') as log, (run / 'forwarded-requests.jsonl').open('x') as fwd:
        p = subprocess.Popen(m['native_command'], cwd=str(run), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, bufsize=1,
                             errors='backslashreplace')  # native errors may quote raw PME bytes
        (run / 'process.json').write_text(json.dumps({
            'driver_pid': os.getpid(), 'native_pid': p.pid, 'binary_sha256': m['artifacts']['okage-native'],
            'manifest_sha256': manifest_sha, 'started_utc': outcome.utc()}) + '\n')
        publish()

        def reader():
            for line in p.stdout:
                log.write(line)
                log.flush()
                if line.startswith('Headless ready frame='):
                    events.put(('ready', line))
                elif line.startswith('{"frame":'):
                    events.put(('status', line))
            events.put(('eof', None))
        threading.Thread(target=reader, daemon=True).start()

        def wait(kind):
            while True:
                k, _ = events.get()
                if k == 'eof':
                    return False
                if k == kind:
                    return True

        def journal(line, origin, **extra):
            rec = {'index': state['acknowledged'], 'origin': origin, 'command': line, 'utc': outcome.utc()}
            rec.update(extra)
            fwd.write(json.dumps(rec) + '\n')
            fwd.flush()

        def send(line, origin):
            journal(line, origin)
            state['acknowledged'] += 1
            try:
                p.stdin.write(line + '\n')
                p.stdin.flush()
            except BrokenPipeError:
                return False
            if line == 'quit':
                return False
            return wait('status' if line == 'status' else 'ready')

        alive = wait('ready')
        for i, line in enumerate(requests):
            if not alive:
                break
            alive = send(line, 'prepared-historical-prefix' if i < historical_count else 'prepared-retained-extension')
            state['alive'] = alive
            publish()
        else:
            if alive:
                state['prepared_complete'] = True
                log.write('DRIVER_PREPARED_REQUESTS_COMPLETE\n')
                log.flush()
                publish()
        offset = 0
        while alive and p.poll() is None:
            data = (run / 'commands.in').read_text()
            if '\n' not in data[offset:]:
                time.sleep(0.05)
                continue
            end = data.rfind('\n', offset) + 1
            chunk, offset = data[offset:end], end
            for raw in chunk.splitlines():
                line = raw.strip()
                if not line or line.startswith('#'):
                    continue
                reason = None
                try:
                    pins.parse_request(line, allow_quit=True)
                except ValueError as e:
                    reason = str(e)
                parts = line.split()
                if reason is None and parts[0] in ('capture', 'view'):
                    if parts[1] in names:
                        reason = 'reused observation name'
                    else:
                        names.add(parts[1])
                if reason is not None:
                    state['rejected'] += 1
                    state['consumed'] += 1  # processed: rejected, never sent
                    journal(line, 'rejected', reason=reason)
                    log.write('DRIVER_REJECTED ' + line + '\n')
                    log.flush()
                    publish()
                    continue
                alive = send(line, 'new-interactive')
                state['consumed'] += 1
                state['alive'] = alive
                publish()
                if not alive:
                    break
        try:
            p.stdin.close()
        except BrokenPipeError:
            pass
        code = p.wait()
        state['alive'] = False
        publish()
    outcome.write_json_atomic(run / 'result.json', {'exit_code': code, 'forwarded': state['acknowledged'],
                                                    'consumed': state['consumed'], 'rejected': state['rejected'],
                                                    'prepared_complete': state['prepared_complete']})
    return code
