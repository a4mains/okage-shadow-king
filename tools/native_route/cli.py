#!/usr/bin/env python3
"""native_route: the single entrypoint for prepared native headless runs.

Run-scoped commands (launch, verify, send, strict-full, route, dev-route, battle, home,
home-inside, restore-route, restore-check) must be executed from the run's own
pinned copy, RUN/tools/native_route/cli.py. Before importing any sibling module
this file (stdlib only) re-executes itself with `python3 -I -B`, checks that it
lives in RUN/tools/native_route, and verifies the sha256 and exact file set of
the pinned package. Siblings are then imported only from RUN/tools (explicit
path; cwd and PYTHONPATH are never consulted), and pins.verify_run() checks the
binary, tapes, prelude inputs, game-data stage identity and Python version.
Any mismatch refuses with exit 10.

Unscoped commands (prepare, launch-new, candidates, compare) may run from the
checkout (WORK/tools/native_route/cli.py).
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PACKAGE = 'native_route'
REFUSED = 10
USAGE = 64
RUN_SCOPED = ('launch', 'verify', 'send', 'strict-full', 'route', 'dev-route', 'battle', 'home', 'home-inside',
              'restore-route', 'restore-check', 'madril', 'restore-madril')

class Parser(argparse.ArgumentParser):
    def error(self, message):  # never exit 2: 2 means prelude-diverged
        self.print_usage(sys.stderr)
        print(f'{self.prog}: error: {message}', file=sys.stderr)
        sys.exit(USAGE)

def refuse(problems, run=None):
    rec = {'refused': True, 'exit_code': REFUSED, 'entrypoint': str(Path(__file__).resolve()),
           'argv': sys.argv[1:], 'problems': list(problems)}
    print(json.dumps(rec, indent=1), file=sys.stderr)
    if run is not None and Path(run).is_dir():
        try:
            with open(Path(run) / 'refusals.jsonl', 'a') as f:
                f.write(json.dumps(rec) + '\n')
        except OSError:
            pass
    sys.exit(REFUSED)

def reexec_isolated():
    if not (sys.flags.isolated and sys.flags.dont_write_bytecode):
        os.execv(sys.executable, [sys.executable, '-I', '-B', str(Path(__file__).resolve())] + sys.argv[1:])

def _sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()

def preverify_tools(run):
    """stdlib-only verification of the pinned package before importing it."""
    run = Path(run).resolve()
    if HERE != run / 'tools' / PACKAGE:
        refuse([f'run-scoped commands must execute {run / "tools" / PACKAGE / "cli.py"}, not {HERE / "cli.py"}'], run)
    try:
        artifacts = json.loads((run / 'manifest.json').read_text())['artifacts']
    except (OSError, ValueError, KeyError, TypeError) as e:
        refuse([f'manifest unreadable: {e}'], run)
    prefix = 'tools/' + PACKAGE + '/'
    pinned = {rel[len(prefix):]: sha for rel, sha in artifacts.items() if rel.startswith(prefix)}
    present = sorted(os.listdir(HERE))
    problems = []
    if present != sorted(pinned):
        problems.append(f'package files {present} != pinned {sorted(pinned)}')
    for name, sha in sorted(pinned.items()):
        p = HERE / name
        if p.is_symlink() or not p.is_file() or _sha(p) != sha:
            problems.append('changed pinned tool ' + name)
    if problems:
        refuse(problems, run)

def import_package():
    """Import native_route only from HERE.parent (explicit path)."""
    tools = HERE.parent
    if PACKAGE in sys.modules:
        refuse(['native_route was imported before bootstrap'])
    sys.path[:] = [str(tools)] + [p for p in sys.path if p and Path(p).resolve() not in (HERE, tools)]
    import importlib
    for name in ('outcome', 'savefmt', 'pins', 'pilot', 'battle', 'encounter', 'navigate', 'route', 'home',
                 'home_inside', 'strict_full', 'restore_route', 'restore_check', 'session_driver',
                 'candidates', 'compare', 'dev_route', 'field_path', 'madril_plan', 'madril', 'madril_save', 'restore_madril'):
        importlib.import_module(PACKAGE + '.' + name)
    for name, mod in list(sys.modules.items()):
        if name == PACKAGE or name.startswith(PACKAGE + '.'):
            if Path(mod.__file__).resolve().parent != HERE:
                refuse([f'{name} imported from {mod.__file__}, not {HERE}'])
    return sys.modules[PACKAGE]

def build_parser():
    ap = Parser(prog='native_route', description='Prepared native headless runs (ordinary input only).')
    sub = ap.add_subparsers(dest='cmd', required=True, parser_class=Parser)

    def prep_args(p, run_arg=True):
        if run_arg:
            p.add_argument('--run', required=True, help='fresh run directory to create')
        p.add_argument('--binary', required=True, help='okage-native executable to copy and pin')
        p.add_argument('--stage', required=True, help='game-data stage directory (identity pinned)')
        p.add_argument('--prepared', required=True,
                       help='prepared-input dir: historical-requests.txt, extension-requests.txt, inputs/{buttons,directions}.txt')
        p.add_argument('--saves', required=True, help='directory of genuine saves to copy and pin')
        p.add_argument('--prelude-ticks', type=int, required=True, help='--frames for the bounded prelude')
        p.add_argument('--newgame-report', action='store_true',
                       help='enable --newgame-report (40-66 GB per run); off by default')
        p.add_argument('--rng-sites', action='store_true',
                       help='pass --rng-sites RUN/rng-sites.jsonl (per-frame RNG draws by placement site; needs a binary with that option)')
        p.add_argument('--event-trace', action='store_true',
                       help='pass --event-trace RUN/event-trace.jsonl (first execution of each field-script pc/opcode; needs a binary with that option)')
        p.add_argument('--min-free-gib', type=int, default=200)
        p.add_argument('--source-repo', help='checkout that built the binary (read-only git HEAD and diff recorded)')

    p = sub.add_parser('prepare', help='create and pin a fresh run directory')
    prep_args(p)
    p = sub.add_parser('launch-new', help='prepare, then start the pinned driver detached')
    prep_args(p)
    p = sub.add_parser('candidates', help='explicit prelude-candidate search running strict-full')
    prep_args(p, run_arg=False)
    p.add_argument('--parent', required=True)
    p.add_argument('--prefix', required=True)
    p.add_argument('--max', type=int, required=True)
    p.add_argument('--target-save', required=True)
    p = sub.add_parser('compare', help='strict input/status comparator (not media equivalence)')
    p.add_argument('rest', nargs=argparse.REMAINDER)

    for name in RUN_SCOPED:
        p = sub.add_parser(name)
        p.add_argument('run')
        if name == 'send':
            p.add_argument('commands', nargs='+')
        if name == 'route':
            p.add_argument('--stop', choices=('prefix', 'walk', 'rosalyn', 'contact'), required=True)
        if name == 'dev-route':
            p.add_argument('--stop', choices=('prefix', 'walk', 'rosalyn', 'contact'), default='contact')
        if name in ('strict-full', 'home-inside'):
            p.add_argument('--target-save', required=True, help='save file the default slot writes, e.g. bkmo2.dat')
            p.add_argument('--tag', default='strict')
        if name == 'strict-full':
            p.add_argument('--prefix-mode', choices=('dev', 'historical'), default='historical',
                           help='historical: unchanged prepared prefix (default); dev: re-time the retained tape '
                                'at the actual bedroom activation (documented deviation, needs pinned dev tapes)')
        if name == 'home':
            p.add_argument('--from-gate', action='store_true')
        if name in ('restore-route', 'restore-check', 'madril', 'restore-madril'):
            p.add_argument('--slot-rights', type=int, required=True)
        if name == 'madril':
            p.add_argument('--slot-file', required=True)
            p.add_argument('--expect-save-sha256', required=True)
            p.add_argument('--no-rest', action='store_true', help='skip the bedroom Rest before leaving')
            p.add_argument('--save-target', help='after the endpoint, Save at the Madril hotel into this slot file')
        if name == 'restore-madril':
            p.add_argument('--slot-file', required=True)
            p.add_argument('--expect-save-sha256', required=True)
        if name == 'restore-check':
            p.add_argument('--slot-file', required=True)
            p.add_argument('--expect-save-sha256', required=True,
                           help='sha256 of the new Save from the strict run (route-result.json); no default')
            p.add_argument('--until-roar', type=int, default=0,
                           help='after the check, keep fighting ordinary encounters until a phase-8 roar battle is won (at most N victories)')
            p.add_argument('--rest-first', action='store_true',
                           help='Rest in the restored bedroom (out to the landing and back) before leaving the house')
            p.add_argument('--rest-every', type=int, default=0,
                           help='with --until-roar: after every N victories walk home and Rest in the bedroom (ordinary heal)')
    return ap

def prepare_kwargs(a):
    return dict(binary=a.binary, stage=a.stage, prepared=a.prepared, saves=a.saves, prelude_ticks=a.prelude_ticks,
                newgame_report=a.newgame_report, rng_sites=a.rng_sites, min_free_gib=a.min_free_gib, source_repo=a.source_repo,
                event_trace=a.event_trace)

def parse(argv):
    ap = build_parser()
    a = ap.parse_args(argv)
    if getattr(a, 'rest_every', 0) and not a.until_roar:
        ap.error('--rest-every requires --until-roar')
    return a

def main(argv=None):
    reexec_isolated()
    argv = sys.argv[1:] if argv is None else list(argv)
    if argv and argv[0] == 'compare':
        return import_package().compare.main(argv[1:])
    a = parse(argv)
    if a.cmd in RUN_SCOPED:
        preverify_tools(a.run)
    nr = import_package()
    pins, outcome = nr.pins, nr.outcome
    try:
        if a.cmd == 'prepare':
            m = nr.session_driver.prepare(a.run, **prepare_kwargs(a))
            print(json.dumps({k: m[k] for k in ('run', 'prepared_requests', 'historical_prefix_count',
                                                'prepared_endpoint', 'newgame_report')}))
            return 0
        if a.cmd == 'launch-new':
            print(json.dumps(nr.candidates.prepare_and_launch(a.run, **prepare_kwargs(a))))
            return 0
        if a.cmd == 'candidates':
            return nr.candidates.run_candidates(a.parent, a.prefix, a.max, a.target_save, prepare_kwargs(a))
        run = Path(a.run).resolve()
        if a.cmd == 'launch':
            return nr.session_driver.launch(run)
        m = pins.verify_run(run, 'execute', a.cmd)
    except pins.PinError as e:
        refuse(e.problems, getattr(a, 'run', None))
    m['_sha256'] = pins.sha256(run / 'manifest.json')
    if a.cmd == 'verify':
        print(json.dumps({'verified': True, 'run': str(run)}))
        return 0
    c = nr.pilot.Ctl(run)
    if a.cmd == 'send':
        for line in a.commands:
            try:
                pins.parse_request(line, allow_quit=True)
            except ValueError as e:
                refuse([str(e)], run)
        for line in c.send(*a.commands):
            print(line)
        return 0
    if a.cmd == 'dev-route' or (a.cmd == 'strict-full' and a.prefix_mode == 'dev'):
        problems = nr.dev_route.pinned_problems(run, m)
        if problems:
            refuse(problems, run)
    if a.cmd == 'strict-full':
        if (run / nr.strict_full.RESULT).exists():
            refuse([f'{nr.strict_full.RESULT} already exists; one strict route per run'], run)
        return nr.strict_full.run_route(c, run, m, a.target_save, a.tag, prefix_mode=a.prefix_mode, argv=argv)
    return run_single(nr, c, run, m, a, argv)

def run_single(nr, c, run, m, a, argv):
    """One stage (or restore-check) with its own RUN/<cmd>-result.json, which
    records the exact argv and, for restore-check, the parsed options."""
    outcome = nr.outcome
    result = run / ('restore-check.json' if a.cmd == 'restore-check' else a.cmd + '-result.json')
    if result.exists():
        refuse([f'{result.name} already exists'], run)
    stages = []

    def record(name, evidence):
        stages.append({'stage': name, 'status': 'PASS', 'evidence': evidence})

    stage = {'route': 'route-prefix', 'dev-route': 'route-dev-prefix', 'battle': 'battle', 'home': 'home',
             'home-inside': 'home-inside',
             'restore-route': 'restore-route', 'restore-check': 'restore-check', 'madril': 'madril',
             'restore-madril': 'restore-madril'}[a.cmd]
    failure = None
    try:
        if a.cmd == 'route':
            nr.route.main(c, run, m, a.stop, record)
        elif a.cmd == 'dev-route':
            nr.route.main(c, run, m, a.stop, record, prefix_mode='dev')
        elif a.cmd == 'battle':
            ev = outcome.run_stage('battle', nr.battle.fight_if_needed, c, run)
            outcome.need(ev is not None, 'battle', 'no battle was active')
            record('battle', ev)
        elif a.cmd == 'home':
            record('home', outcome.run_stage('home', nr.home.main, c, run, a.from_gate))
        elif a.cmd == 'home-inside':
            outcome.run_stage('home-inside', nr.home_inside.main, c, run, m, a.target_save, a.tag, record)
        elif a.cmd == 'restore-route':
            record('restore-route', outcome.run_stage('restore-route', nr.restore_route.main, c, run, m, a.slot_rights))
        elif a.cmd == 'madril':
            outcome.run_stage('madril-arrival', nr.madril.main, c, run, m, a.slot_file, a.slot_rights,
                              a.expect_save_sha256, record, not a.no_rest)
            if a.save_target:
                outcome.run_stage('save', nr.madril_save.main, c, run, m, a.save_target, record)
        elif a.cmd == 'restore-madril':
            outcome.run_stage('restore-check', nr.restore_madril.main, c, run, m, a.slot_file, a.slot_rights,
                              a.expect_save_sha256, record)
        elif a.cmd == 'restore-check':
            outcome.run_stage('restore-check', nr.restore_check.main, c, run, m, a.slot_file, a.slot_rights,
                              a.expect_save_sha256, record, a.until_roar, a.rest_every, a.rest_first)
    except outcome.StageFailure as e:
        failure = e
    extra = {'manifest_sha256': m['_sha256'], 'stage': stage, 'argv': list(argv),
             **({'prefix_mode': 'dev'} if a.cmd == 'dev-route' else {})}
    if a.cmd in ('restore-check', 'restore-madril'):
        extra['options'] = {k: getattr(a, k, None) for k in ('slot_file', 'slot_rights', 'expect_save_sha256',
                                                       'until_roar', 'rest_every', 'rest_first')}
    code = outcome.write_result(result, a.cmd, stages, failure, extra)
    print(a.cmd, 'exit', code, failure.record() if failure else 'PASS', flush=True)
    return code

if __name__ == '__main__':
    sys.exit(main())
