#!/usr/bin/env python3
"""Audit the first village conversation opening after native VIL bank replacement.

Compares119 controller returns and manager states from event32/message10008.
It does not establish closure, full camera/actor output, UI pixels or audio.
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
    write_exact(a.output,(json.dumps(report,indent=2)+'\n').encode())
    print('Village event32/message10008:119 controller returns and119 manager entries exact')

if __name__ == '__main__':
    main()
