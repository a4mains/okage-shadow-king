"""Shared synthetic fixtures for the native_route tests (no TestCase here).

Nothing here needs the native binary or owned assets: runs, stages, saves and
the native process itself are synthetic.
"""
import json
import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from native_route import savefmt  # noqa: E402

CLI = TOOLS / 'native_route' / 'cli.py'

def save_bytes(seed):
    payload = bytes((seed * 31 + i) & 0xff for i in range(savefmt.PAYLOAD_SIZE))
    return savefmt.encode(payload)

def tape_held(tapes, frame):
    value = 0
    for tape in tapes:
        cur = 0
        for f, m in tape:
            if f <= frame:
                cur = m
        value |= cur
    return value

def make_compare_run(root, prelude=6, requests=None, buttons=((1, 0), (3, 0x40), (4, 0)),
                     directions=((1, 0), (5, 0x1000))):
    """A self-consistent run: prelude tapes, journaled requests, input journal
    and stdout.log, as the driver and native write them."""
    root = Path(root)
    (root / 'inputs').mkdir(parents=True)
    requests = list(requests if requests is not None else
                    ['step 3 1000', 'status', 'status', 'step 2 0040', 'status', 'step 4 0000', 'status'])
    (root / 'manifest.json').write_text(json.dumps({'prelude_ticks': prelude, 'artifacts': {'okage-native': 'f' * 64}}))
    (root / 'inputs' / 'buttons.txt').write_text(''.join(f'{f} {m:04x}\n' for f, m in buttons))
    (root / 'inputs' / 'directions.txt').write_text(''.join(f'{f} {m:04x}\n' for f, m in directions))
    journal = [{'kind': 'header', 'schema': 'okage-native-input-journal-v1', 'version': 1}]
    log = ['Native startup constructed selector=3 frame=2']
    prev = 0

    def tick(frame, held):
        nonlocal prev
        journal.append({'kind': 'tick_attempt', 'frame': frame, 'held': held, 'pressed': held & ~prev & 0xffff})
        journal.append({'kind': 'tick_complete', 'frame': frame})
        prev = held

    for f in range(1, prelude + 1):
        tick(f, tape_held([buttons, directions], f))
    frame = prelude
    journal.append({'kind': 'pause', 'frame': frame})
    log += [f'Native frames={frame} progress=0 wait_release=0', f'Headless ready frame={frame} scene=1 pc=0 message=0']
    fwd = []
    for i, r in enumerate(requests):
        fwd.append({'index': i, 'origin': 'prepared-historical-prefix', 'command': r, 'utc': 'x'})
        parts = r.split()
        if parts[0] == 'step':
            journal.append({'kind': 'resume', 'frame': frame})
            for _ in range(int(parts[1])):
                frame += 1
                tick(frame, int(parts[2], 16))
            journal.append({'kind': 'pause', 'frame': frame})
            log.append(f'Headless ready frame={frame} scene=1 pc=0 message=0')
        elif r == 'status':
            log.append(json.dumps({'frame': frame, 'scene': 1, 'entrance': -1, 'event_running': False, 'pc': 512,
                                   'message': 0, 'choice': 0, 'camera_yaw': 0.25,
                                   'hero_position': [1.5, 0.0, float(frame)], 'guard_pending_opcode': 0},
                                  separators=(',', ':')))
    (root / 'inputs.jsonl').write_text(''.join(json.dumps(j, separators=(',', ':')) + '\n' for j in journal))
    (root / 'stdout.log').write_text('\n'.join(log) + '\n')
    (root / 'forwarded-requests.jsonl').write_text(''.join(json.dumps(x) + '\n' for x in fwd))
    return root

def lines(path):
    return Path(path).read_text().splitlines()

def write_lines(path, items):
    Path(path).write_text('\n'.join(items) + '\n')

FAKE_NATIVE = r'''#!{python}
"""Synthetic stand-in for okage-native --headless-control (tests only)."""
import json, os, sys
args = sys.argv[1:]
def opt(name):
    return args[args.index(name) + 1] if name in args else None
frames = int(opt('--frames'))
control = opt('--headless-control')
os.mkdir(control)
def tape(path):
    out = []
    for line in open(path):
        line = line.strip()
        if line and not line.startswith('#'):
            f, m = line.split(); out.append((int(f), int(m, 16)))
    return out
tapes = [tape(opt('--buttons')), tape(opt('--directions'))]
def held_at(f):
    v = 0
    for t in tapes:
        cur = 0
        for ff, m in t:
            if ff <= f: cur = m
        v |= cur
    return v
journal = open(opt('--input-journal'), 'x')
def j(**r):
    journal.write(json.dumps(r, separators=(',', ':')) + '\n'); journal.flush()
j(kind='header', schema='okage-native-input-journal-v1', version=1)
prev = 0
frame = 0
def tick(h):
    global prev, frame
    frame += 1
    j(kind='tick_attempt', frame=frame, held=h, pressed=h & ~prev & 0xffff); j(kind='tick_complete', frame=frame)
    prev = h
for f in range(1, frames + 1):
    tick(held_at(f))
j(kind='pause', frame=frame)
def out(s):
    sys.stdout.write(s + '\n'); sys.stdout.flush()
out('Native frames=%d progress=0 wait_release=0' % frames)
def ready():
    out('Headless ready frame=%d scene=1 pc=0 message=0' % frame)
ready()
for line in sys.stdin:
    line = line.rstrip('\n')
    p = line.split()
    if line == 'quit':
        break
    if line == 'status':
        out(json.dumps({'frame': frame, 'scene': 1, 'entrance': -1, 'event_running': False, 'pc': 512, 'message': 0,
                        'choice': 0, 'camera_yaw': 0.25, 'hero_position': [1.0, 0.0, float(frame)],
                        'guard_pending_opcode': 0}, separators=(',', ':')))
        continue
    if line == 'observe':
        out(json.dumps({'observe_frame': frame, 'game_mode': 2, 'field': True, 'battle': False, 'actors': []}))
    elif p and p[0] in ('view', 'capture') and len(p) == 2:
        open(os.path.join(control, p[1] + ('.view.png' if p[0] == 'view' else '.png')), 'x').close()
    elif p and p[0] == 'step' and len(p) == 3:
        j(kind='resume', frame=frame)
        for _ in range(int(p[1])):
            tick(int(p[2], 16))
        j(kind='pause', frame=frame)
    else:
        sys.stderr.write('Expected: step TICKS HEXMASK, capture NAME, status, observe, view NAME, or quit\n')
        sys.exit(1)
    ready()
'''

def make_fake_native(path):
    path = Path(path)
    path.write_text(FAKE_NATIVE.replace('{python}', sys.executable))
    path.chmod(0o755)
    return path

def make_stage(root):
    """Synthetic game-data stage with a symlinked NATIVE_UI file."""
    root = Path(root)
    owned = root / 'owned'
    owned.mkdir(parents=True)
    (owned / 'battle-ui.bin').write_bytes(b'ui-v1')
    stage = root / 'stage-real'
    (stage / 'NATIVE_UI').mkdir(parents=True)
    (stage / 'BTLDATA' / 'ALPHA').mkdir(parents=True)
    (stage / '.owned-game-data-manifest.json').write_text('{"file_count": 1}\n')
    os.symlink(owned / 'battle-ui.bin', stage / 'NATIVE_UI' / 'battle-ui.bin')
    (stage / 'NATIVE_UI' / 'plain.json').write_text('{}\n')
    link = root / 'stage-link'
    os.symlink(stage, link)
    return link, owned

def make_prepared(root, historical=None, extension=None):
    root = Path(root)
    (root / 'inputs').mkdir(parents=True)
    hist = historical if historical is not None else ['step 3 1000', 'status', 'view hist-a']
    ext = extension if extension is not None else ['step 2 0040', 'status']
    (root / 'historical-requests.txt').write_text(''.join(l + '\n' for l in hist))
    (root / 'extension-requests.txt').write_text(''.join(l + '\n' for l in ext))
    (root / 'inputs' / 'buttons.txt').write_text('1 0000\n3 0040\n4 0000\n')
    (root / 'inputs' / 'directions.txt').write_text('1 0000\n')
    return root

def make_saves(root):
    root = Path(root)
    root.mkdir(parents=True)
    for i in range(3):
        (root / f'bkmo{i}.dat').write_bytes(save_bytes(i))
    return root
