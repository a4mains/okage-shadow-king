#!/usr/bin/env python3
"""Compare bakery event30/message10015, with optional answers and confirmation lock.

Uses observed event dispatch and pad polling. Does not establish bakery camera,
rendering, audio, jingle playback or conversation completion.
"""
import argparse
import json
import struct
from pathlib import Path
from prepare_assets import ELF_HASH
from verify_event import digest, write_exact
from verify_message_opening import require, rows, compare_controller

HASHES = {
    '.controller.bin': '91e0ab74693adf9b13c53c8f6769f3b46f3754eb1830c6711e4a5e2c0abf16bc',
    '.controller.bin.choices.bin': 'a5b0edf0b7140f408430bc2461eeaf1a28612826338d19461de58e5e5daf6f41',
    '.transition.bin': '2e830f2060958de836c927a229c17489b28a9b5c14ed47a7106519e6c427c649',
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--answer', action='store_true', help='Compare both first answers and their following dialogue')
    parser.add_argument('--lock', action='store_true', help='Compare confirmation disabling through the tick before the jingle')
    args = parser.parse_args()
    require(digest(Path('local/extracted/SCUS_971.29')) == ELF_HASH, 'Different retail ELF')
    prefix = 'local/reference-bakery-talk-480-v1'
    original = lambda suffix: Path(prefix + suffix)
    native = lambda suffix: Path(str(args.capture) + suffix)
    original_run = json.loads(original('.run.json').read_text())
    native_run = json.loads(Path(str(args.capture).removesuffix('.png') + '.run.json').read_text())
    require(original_run['exit_code'] == native_run['exit_code'] == 0, 'Incomplete capture')
    for suffix, sha in HASHES.items():
        require(digest(original(suffix)) == sha, 'Changed original bakery corpus')
    live = rows(native('.interaction.bin').read_bytes(), 8)
    total = len(live)
    require(total >= 13539 and [r[0] for r in live] == list(range(1, total + 1)), 'Native extent')
    observed = rows(original('.transition.bin').read_bytes(), 128)
    require([(r[0], r[2], r[3]) for r in observed if r[1] == 0x1bb7e0] ==
            [(51, 0x132c14, 30)], 'Different original event dispatch')
    require(live[13110][2:5] == (0xfffffff8, 30, 1), 'Different native NPC/event')
    words = compare_controller(original('.controller.bin').read_bytes(), native('.dialogue.bin').read_bytes(),
        first_original=51, first_native=13111, frames=429, total=total, message_id=10015)
    controllers = rows(original('.controller.bin').read_bytes(), 64)[1::2]
    pressed = []
    for r in controllers:
        require(live[13059+r[0]][1] == r[9], 'Observed pad differs at ' + str(r[0]))
        if r[9]:
            pressed.append(r[0])
    require(pressed == [51, 222, 263, 304, 344, 385], 'Different original confirmations')
    managers = [r for r in observed if r[1] == 0x133a98 and r[0] >= 52]
    require([r[0] for r in managers] == list(range(52, 480)), 'Manager interval')
    for r in managers:
        n = live[13058+r[0]]
        require((n[4], n[7], n[5], n[6]) ==
                (int(bool(r[35]&256)), r[36], r[48], r[118]), 'Bakery manager differs')
    choices = rows(original('.controller.bin.choices.bin').read_bytes(), 16)[1::2]
    native_choices = rows(native('.choices.bin').read_bytes(), 12)
    require([r[0] for r in choices] == list(range(51, 480)), 'Choice interval')
    for r in choices:
        require(r[2:6] == native_choices[13059+r[0]][2:6], 'Choice state differs')
    require(native_choices[13538][6:8] == (1, 0), 'First choice is not waiting unarmed')
    files = [original(suffix) for suffix in HASHES]
    files += [native(suffix) for suffix in ['.interaction.bin', '.dialogue.bin', '.choices.bin']]
    report = dict(scope=__doc__, original_run=original_run, native_run=native_run,
        original_dispatch=51, native_dispatch=13111, controller_returns=429,
        exact_controller_words=words, manager_entries=428, exact_manager_words=1712,
        choice_states=429, exact_choice_words=1716, observed_confirmations=pressed,
        sha256={str(p): digest(p) for p in files})
    if args.answer:
        answer = 'local/reference-bakery-answer-600-v1'
        paths = lambda suffix: Path(answer+suffix)
        for suffix, sha in {
            '.controller.bin': '142855d5a13f43cdfbffce9de69ee1e474440eda533924f044a9dac729975f9b',
            '.controller.bin.choices.bin': 'f8d7ebb85a59194b87da8ed0a239e560011086a5fecef567e88f6894ae708d8d',
            '.transition.bin': 'f7785bcd0e3b2a720e330750b187e34c32a76885f59ecda58d588b4f6090f52b',
        }.items():
            require(digest(paths(suffix)) == sha, 'Changed original answer corpus')
            report['sha256'][str(paths(suffix))] = sha
        run = json.loads(paths('.run.json').read_text())
        require(run['exit_code'] == 0 and total >= 14139, 'Incomplete answer comparison')
        count = compare_controller(paths('.controller.bin').read_bytes(), native('.dialogue.bin').read_bytes(),
            first_original=0, first_native=13540, frames=600, total=total, message_id=10015)
        confirms = []
        for r in rows(paths('.controller.bin').read_bytes(), 64)[1::2]:
            require(live[13539+r[0]][1] == r[9], 'Answer input differs')
            if r[9] == 0x40:
                confirms.append(r[0])
        require(confirms == [21,62,103,144,185,226,301,342,383,424,465,506,548,589],
                'Answer confirmation corpus differs')
        managers = [r for r in rows(paths('.transition.bin').read_bytes(), 128) if r[1] == 0x133a98]
        require([r[0] for r in managers] == list(range(600)), 'Answer manager extent')
        for r in managers:
            n = live[13538+r[0]]
            require((n[4], n[7], n[5], n[6]) ==
                    (int(bool(r[35]&256)), r[36], r[48], r[118]), 'Answer manager differs')
        choices = rows(paths('.controller.bin.choices.bin').read_bytes(), 16)[1::2]
        require([r[0] for r in choices] == list(range(600)), 'Answer choice extent')
        for r in choices:
            require(r[2:6] == native_choices[13539+r[0]][2:6], 'Answer choice differs')
        accepts = [(r[0], r[5]) for i,r in enumerate(native_choices[13539:14139],13539)
                   if native_choices[i-1][7] and not r[7]]
        require(accepts == [(13561,0),(13841,0)], 'Different accepted answers')
        report['answers'] = dict(original_run=run, controller_returns=600, exact_controller_words=count,
            manager_entries=600, exact_manager_words=2400, choice_states=600, exact_choice_words=2400,
            accepted_answers=accepts, observed_confirmations=confirms)
    if args.lock:
        require(args.answer and total >= 14245, 'Lock comparison requires answers and14245 frames')
        lock = 'local/reference-bakery-lock-300-v1'
        paths = lambda suffix: Path(lock+suffix)
        for suffix, sha in {
            '.controller.bin': '16cabbacdb0742cfa475f85e7218a3f33b084d96afa8ed58da776cd050a57d48',
            '.transition.bin': '082949995376e41d1351d7decebe6e6a8947e348cb0c20dbc604f63530e4c57b',
        }.items():
            require(digest(paths(suffix)) == sha, 'Changed original lock corpus')
            report['sha256'][str(paths(suffix))] = sha
        run = json.loads(paths('.run.json').read_text())
        require(run['exit_code'] == 0, 'Incomplete original lock capture')
        controller = rows(paths('.controller.bin').read_bytes(), 64)
        interval = [r for r in controller if r[0] < 106]
        data = b''.join(struct.pack('<64I', *r) for r in interval)
        count = compare_controller(data, native('.dialogue.bin').read_bytes(), first_original=0,
            first_native=14140, frames=106, total=total, message_id=10015)
        for r in interval[1::2]:
            require(live[14139+r[0]][1] == r[9], 'Lock input differs')
        flags = [(r[0],r[17]) for i,r in enumerate(controller[1::2])
                 if not i or r[17] != controller[2*i-1][17]]
        require(flags == [(0,0x70),(35,0x60),(287,0x70)], 'Original confirmation-toggle boundaries differ')
        managers = [r for r in rows(paths('.transition.bin').read_bytes(), 128)
                    if r[1] == 0x133a98 and r[0] < 106]
        require([r[0] for r in managers] == list(range(106)), 'Lock manager extent')
        for r in managers:
            n = live[14138+r[0]]
            require((n[4], n[7], n[5], n[6]) ==
                    (int(bool(r[35]&256)), r[36], r[48], r[118]), 'Lock manager differs')
        report['lock'] = dict(original_run=run, controller_returns=106, exact_controller_words=count,
            manager_entries=106, exact_manager_words=424, native_disable_frame=14175,
            original_disable_frame=35, original_enable_frame=287,
            limit='Native re-enable not yet reached; jingle remains unimplemented')
    write_exact(args.output, (json.dumps(report, indent=2)+'\n').encode())
    print('Bakery opening exact; answers compared:',args.answer,'; confirmation lock compared:',args.lock)

if __name__ == '__main__':
    main()
