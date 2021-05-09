#!/usr/bin/env python3
"""Audit the first village conversation opening after native VIL bank replacement.

Compares119 opening controller returns and manager states for event32/message10008.
Optional ending adds262 controller returns and300 manager entries through closure.
Full camera/actor output, UI pixels and audio remain outside this comparison.
"""
import argparse
import json
from pathlib import Path
from prepare_assets import ELF_HASH
from verify_event import digest, write_exact
from verify_message_opening import require, rows, compare_controller

HASHES = {'.controller.bin':'7dd2d2f0fa557f5d4136d94f93dd7e8c70d35567c2686916b685ed4a5c118b17',
          '.transition.bin':'916dfe8ff1374d68f155a972d8a2a85ed243974378cf363504c16a40637ede44'}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--ending',action='store_true')
    a = p.parse_args()
    require(digest(Path('local/extracted/SCUS_971.29')) == ELF_HASH, 'Different retail ELF')
    prefix = 'local/reference-village-talk-260-v1'
    paths = [Path(prefix+s) for s in HASHES]
    for suffix,sha in HASHES.items():
        require(digest(Path(prefix+suffix)) == sha, 'Different original dialogue corpus')
    original_run = json.loads(Path(prefix+'.run.json').read_text())
    native_run = json.loads(Path(str(a.capture).removesuffix('.png')+'.run.json').read_text())
    require(original_run['exit_code'] == native_run['exit_code'] == 0, 'Incomplete dialogue run')
    dialogue = Path(str(a.capture)+'.dialogue.bin')
    interaction = Path(str(a.capture)+'.interaction.bin')
    paths += [dialogue,interaction]
    live = rows(interaction.read_bytes(),8)
    require([r[0] for r in live] == list(range(1,len(live)+1)), 'Native frame extent')
    observed = rows(Path(prefix+'.transition.bin').read_bytes(),128)
    require([(r[0],r[3]) for r in observed if r[1] == 0x1bb7e0] == [(141,32)], 'Original event selection')
    require(live[12100][2:5] == (0xfffffff6,32,1), 'Native event selection')
    words = compare_controller(Path(prefix+'.controller.bin').read_bytes(),dialogue.read_bytes(),
        first_original=141,first_native=12101,frames=119,total=len(live),message_id=10008)
    manager = [r for r in observed if r[1] == 0x133a98 and r[0] >= 141]
    require([r[0] for r in manager] == list(range(141,260)), 'Manager observation extent')
    for r in manager:
        got = live[11958+r[0]]
        require((got[4],got[7],got[5],got[6]) ==
                (int(bool(r[35]&256)),r[36],r[48],r[118]), 'Village manager differs')
    report = dict(scope=__doc__, original_run=original_run,native_run=native_run,
        controller_returns=119,exact_controller_words=words,manager_entries=119,
        exact_manager_words=476,native_dispatch=12101,original_dispatch=141,
        sha256={str(path):digest(path) for path in paths})
    if a.ending:
        end = 'local/reference-village-talk-end-300-v1'
        for suffix,sha in [('.controller.bin','71d433f48a12bca7105dcfb88ca7a4f46a1d010c6721623042fa04a1bcc8b954'),
                           ('.transition.bin','56e425aacac9aeee92e66dbf5a731ebb95f31b2c7c8b4703a120c3df3215b329')]:
            require(digest(Path(end+suffix)) == sha, 'Different original ending corpus')
            report['sha256'][end+suffix] = sha
        end_run = json.loads(Path(end+'.run.json').read_text())
        require(end_run['exit_code'] == 0 and len(live)>=12520, 'Incomplete closure interval')
        ending_controller = Path(end+'.controller.bin').read_bytes()
        end_words = compare_controller(ending_controller,dialogue.read_bytes(),first_original=0,
            first_native=12220,frames=262,total=len(live),message_id=10008)
        pressed = []
        for r in rows(ending_controller,64)[1::2]:
            require(live[12219+r[0]][1] == r[9], 'Observed closing input differs')
            if r[9]:pressed.append(r[0])
        require(pressed == [1,42,83,124,165,222], 'Unexpected original confirmations')
        end_manager = [r for r in rows(Path(end+'.transition.bin').read_bytes(),128) if r[1]==0x133a98]
        require([r[0] for r in end_manager] == list(range(300)), 'Ending manager extent')
        for r in end_manager:
            got = live[12218+r[0]]
            require((got[4],got[7],got[5],got[6]) ==
                    (int(bool(r[35]&256)),r[36],r[48],r[118]), 'Ending manager differs')
        text = rows(dialogue.read_bytes(),12)
        require(text[12479][10] == 9 and live[12483][4:7] == (0,1,1)
                and live[12484][4:7] == (0,0,1), 'Native closure/control boundary differs')
        report['ending'] = dict(original_run=end_run,controller_returns=262,
            exact_controller_words=end_words,manager_entries=300,exact_manager_words=1200,
            observed_confirmations=pressed,stage9_frame=12480,event_end=12484,control_return=12485)
    write_exact(a.output,(json.dumps(report,indent=2)+'\n').encode())
    print('Village event32/message10008 opening exact; ending compared:',bool(a.ending))

if __name__ == '__main__':
    main()
