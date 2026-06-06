"""Run pinning: the complete transitive input/tool set of one prepared run.

A run directory holds copies of the binary, request tapes, prelude button and
direction tapes, initial saves and this whole package (RUN/tools/native_route).
manifest.json pins every one by sha256, plus the game-data stage identity and
the Python version. verify_run() recomputes all of it at every execution and
refuses on any difference.
"""
import hashlib
import json
import os
import platform
import re
import sys
from pathlib import Path

from . import outcome

PACKAGE = 'native_route'
TOOLS_REL = 'tools/' + PACKAGE
TAPES = ('historical-requests.txt', 'extension-requests.txt')
PRELUDE_INPUTS = ('inputs/buttons.txt', 'inputs/directions.txt')
LAUNCH_OUTPUTS = ('inputs.jsonl', 'reads.jsonl', 'control', 'report', 'stdout.log', 'process.json',
                  'forwarded-requests.jsonl', 'driver-state.json', 'result.json', 'battle-live.jsonl')
PACKAGE_SUFFIXES = ('.py', '.md')
NAME = r'[A-Za-z0-9_-]{1,64}'
REQUEST = re.compile(r'^(step ([1-9][0-9]{0,3}) ([0-9a-fA-F]{1,4})|status|observe|(capture|view) (' + NAME + r'))$')
TAPE_LINE = re.compile(r'^([1-9][0-9]*) ([0-9a-fA-F]{1,4})$')

class PinError(Exception):
    def __init__(self, problems):
        super().__init__('; '.join(problems))
        self.problems = list(problems)

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()

def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'))

def python_identity():
    return {'version': platform.python_version(), 'implementation': platform.python_implementation(),
            'executable': sys.executable}

def parse_request(line, allow_quit=False):
    """Return the normalized request or raise ValueError. Only pad steps and
    read-only observation commands are accepted (no state/seed/timing input)."""
    if allow_quit and line == 'quit':
        return line
    m = REQUEST.match(line)
    if not m:
        raise ValueError('not an ordinary pad/observation request: ' + repr(line))
    if m.group(2) is not None:
        ticks = int(m.group(2))
        if ticks > 6000:
            raise ValueError('step ticks > 6000: ' + repr(line))
    if m.group(5) is not None and m.group(5).lower() == 'final':
        raise ValueError('reserved observation name: ' + repr(line))
    return line

def read_tape(path):
    lines = Path(path).read_text().splitlines()
    for i, line in enumerate(lines):
        try:
            parse_request(line)
        except ValueError as e:
            raise PinError([f'{path}:{i + 1}: {e}'])
    return lines

def read_prelude_tape(path):
    out = []
    last = 0
    for i, raw in enumerate(Path(path).read_text().splitlines()):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        m = TAPE_LINE.match(line)
        if not m or int(m.group(1)) <= last:
            raise PinError([f'{path}:{i + 1}: expected increasing "FRAME HEXMASK"'])
        last = int(m.group(1))
        out.append((last, int(m.group(2), 16)))
    return out

def step_ticks(lines):
    return sum(int(s.split()[1]) for s in lines if s.startswith('step '))

def stage_identity(stage):
    """Identity of a game-data stage: manifest hash, resolved listing of the top
    two levels (symlink text and resolved target) and every NATIVE_UI file hash."""
    given = Path(stage)
    root = given.resolve()
    manifest = root / '.owned-game-data-manifest.json'
    if not manifest.is_file():
        raise PinError([f'stage {given} lacks .owned-game-data-manifest.json'])
    ui = root / 'NATIVE_UI'
    if not ui.is_dir():
        raise PinError([f'stage {given} lacks NATIVE_UI'])

    def entry(path, rel):
        e = {'path': rel}
        if path.is_symlink():
            e['kind'] = 'symlink'
            e['link'] = os.readlink(path)
        elif path.is_dir():
            e['kind'] = 'dir'
        elif path.is_file():
            e['kind'] = 'file'
        else:
            e['kind'] = 'other'
        target = Path(os.path.realpath(path))
        e['target'] = str(target)
        e['target_kind'] = 'dir' if target.is_dir() else 'file' if target.is_file() else 'missing'
        if e['target_kind'] == 'file':
            e['size'] = target.stat().st_size
        return e

    listing = []
    for first in sorted(os.listdir(root)):
        p = root / first
        listing.append(entry(p, first))
        if p.is_dir():
            for second in sorted(os.listdir(p)):
                listing.append(entry(p / second, first + '/' + second))
    native_ui = []
    for dirpath, dirnames, filenames in os.walk(ui, followlinks=True):
        dirnames.sort()
        for name in sorted(filenames):
            p = Path(dirpath) / name
            target = Path(os.path.realpath(p))
            if not target.is_file():
                raise PinError([f'NATIVE_UI entry {p} does not resolve to a file'])
            native_ui.append({'path': str(p.relative_to(ui)), 'target': str(target),
                              'size': target.stat().st_size, 'sha256': sha256(target)})
    ident = {'given_path': str(given), 'resolved_path': str(root),
             'manifest_sha256': sha256(manifest), 'top_two_levels': listing, 'native_ui': native_ui}
    ident['identity_sha256'] = hashlib.sha256(canonical(ident).encode()).hexdigest()
    return ident

def package_dir():
    return Path(__file__).resolve().parent

def package_files(directory, source=False):
    """Every file the package directory contains. Anything other than a regular
    .py/.md file is refused so nothing unpinned can be imported or executed.
    A checkout (source=True) may hold a bytecode cache, which is never copied;
    a run's pinned copy may not."""
    out = []
    for p in sorted(Path(directory).iterdir()):
        if p.name == '__pycache__' and source:
            continue
        if p.name == '__pycache__':
            raise PinError([f'{p}: bytecode cache present; tools must run from pinned source only'])
        if p.is_symlink() or not p.is_file() or p.suffix not in PACKAGE_SUFFIXES:
            raise PinError([f'{p}: unexpected entry in tool package'])
        out.append(p.name)
    return out

def manifest_path(run):
    return Path(run) / 'manifest.json'

def load_manifest(run):
    try:
        return json.loads(manifest_path(run).read_text())
    except (OSError, ValueError) as e:
        raise PinError([f'manifest unreadable: {e}'])

def verify_run(run, phase, entrypoint=None):
    """Recompute every pin. phase 'launch' additionally requires untouched
    initial saves and no prior outputs; any later phase requires the launch
    record to name this manifest's hash. Raises PinError listing all problems."""
    run = Path(run).resolve()
    m = load_manifest(run)
    problems = []
    if m.get('schema') != 'native-route-run-v1':
        problems.append('manifest schema is not native-route-run-v1')
    if m.get('run') != str(run):
        problems.append(f'manifest run {m.get("run")} != {run}')
    artifacts = m.get('artifacts') or {}
    for rel, pin in sorted(artifacts.items()):
        p = run / rel
        if not p.is_file() or p.is_symlink():
            problems.append('missing pinned artifact ' + rel)
        elif sha256(p) != pin:
            problems.append('changed pinned artifact ' + rel)
    try:
        present = package_files(run / TOOLS_REL)
        pinned = sorted(rel.split('/')[-1] for rel in artifacts if rel.startswith(TOOLS_REL + '/'))
        if present != pinned:
            problems.append(f'tool package files {present} != pinned {pinned}')
    except (PinError, OSError) as e:
        problems.append(str(e))
    for required in TAPES + PRELUDE_INPUTS + ('okage-native',):
        if required not in artifacts:
            problems.append('manifest does not pin ' + required)
    try:
        lines = (run / TAPES[0]).read_text().splitlines()
        if m.get('historical_prefix_count') != len(lines):
            problems.append('historical_prefix_count does not equal the pinned historical tape length')
    except OSError as e:
        problems.append(str(e))
    stage = m.get('game_data_stage') or {}
    try:
        now = stage_identity(stage.get('given_path', ''))
        if now != stage:
            changed = [k for k in sorted(set(now) | set(stage)) if now.get(k) != stage.get(k)]
            problems.append('game-data stage identity changed: ' + ', '.join(changed))
    except (PinError, OSError) as e:
        problems.append('game-data stage: ' + str(e))
    py = python_identity()
    pinned_py = m.get('python') or {}
    if (py['version'], py['implementation']) != (pinned_py.get('version'), pinned_py.get('implementation')):
        problems.append(f'python {py["version"]} {py["implementation"]} != pinned '
                        f'{pinned_py.get("version")} {pinned_py.get("implementation")}')
    if phase == 'launch':
        for name, pin in sorted((m.get('initial_saves') or {}).items()):
            p = run / 'saves' / name
            if not p.is_file() or sha256(p) != pin['sha256']:
                problems.append('changed initial save ' + name)
        extra = sorted(set(os.listdir(run / 'saves')) - set(m.get('initial_saves') or {})) if (run / 'saves').is_dir() else ['<saves missing>']
        if extra:
            problems.append(f'unpinned save entries {extra}')
        for name in LAUNCH_OUTPUTS:
            if (run / name).exists():
                problems.append('existing output ' + name)
    else:
        try:
            proc = json.loads((run / 'process.json').read_text())
            if proc.get('manifest_sha256') != sha256(manifest_path(run)):
                problems.append('manifest.json changed after launch')
        except (OSError, ValueError):
            problems.append('run was not launched by the pinned driver (process.json missing)')
    if problems:
        raise PinError(problems)
    rec = {'utc': outcome.utc(), 'entrypoint': entrypoint, 'phase': phase, 'pid': os.getpid(),
           'verified_artifacts': len(artifacts), 'stage_identity_sha256': stage.get('identity_sha256')}
    with open(run / 'verifications.jsonl', 'a') as f:
        f.write(json.dumps(rec) + '\n')
    return m
