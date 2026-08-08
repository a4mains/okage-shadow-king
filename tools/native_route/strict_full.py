"""Strict-prefix route for one prepared run: unchanged prepared prefix, ordinary
walk, natural Chapter 2 contact, ordinary battle, walk home, climb, Save.

Every stage must reach its verified postcondition; the first failure stops the
route, is written to RUN/route-result.json with its stage, code and reason, and
becomes the exit code. Exit 0 only when every stage, including the Save
postconditions, passed.
"""
from . import battle, home, home_inside, route
from .outcome import StageFailure, need, run_stage, write_result

RESULT = 'route-result.json'

def run_route(c, run, manifest, target_save, tag='strict', result_name=RESULT, entrypoint='strict-full',
              prefix_mode='historical', argv=None):
    stages = []

    def record(name, evidence):
        stages.append({'stage': name, 'status': 'PASS', 'evidence': evidence})

    failure = None
    try:
        run_stage('route-prefix', route.main, c, run, manifest, 'contact', record,
                  prefix_mode=prefix_mode)
        ev = run_stage('battle', battle.fight_if_needed, c, run)
        need(ev is not None, 'battle', 'no battle followed the natural contact')
        record('battle', ev)
        record('home', run_stage('home', home.main, c, run))
        run_stage('home-inside', home_inside.main, c, run, manifest, target_save, tag, record)
    except StageFailure as e:
        failure = e
    code = write_result(run / result_name, entrypoint, stages, failure,
                        {'target_save': target_save, 'prefix_mode': prefix_mode, 'manifest_sha256': manifest.get('_sha256'),
                         **({'argv': list(argv)} if argv is not None else {})})
    print(entrypoint, 'exit', code, failure.record() if failure else 'PASS', flush=True)
    return code
