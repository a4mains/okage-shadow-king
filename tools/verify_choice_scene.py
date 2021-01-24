#!/usr/bin/env python3
"""Audit completed choice captures; never substitutes captured data into the app."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from verify_event import write_exact

def rows(path, count):
    return list(struct.iter_unpack('<%dI' % count, path.read_bytes()))

def same_family(first, second, suffixes):
    for suffix in suffixes:
        a, b = Path(str(first)+suffix), Path(str(second)+suffix)
        if a.read_bytes() != b.read_bytes():
            raise RuntimeError(f'Capture mismatch: {a} versus {b}')

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, default=Path('local/choice-scene-validation.json'))
    args = p.parse_args()
    prefix = Path('local/native-choice-third-4080-v1.png')
    reference_path = Path('local/reference-choice-select-v1.controller.bin.choices.bin')
    reference = rows(reference_path, 16)
    if len(reference) != 320:
        raise ValueError('Expected 160 original controller entry/return pairs')
    for i in range(160):
        if reference[i*2][:2] != (i, 0x148dc8) or reference[i*2+1][:2] != (i, 0x11ff38):
            raise ValueError('Original controller pairing mismatch')
    choices = rows(Path(str(prefix)+'.choices.bin'), 12)
    dialogue = rows(Path(str(prefix)+'.dialogue.bin'), 12)
    entries = [r for r in reference if r[1] == 0x11ff38 and r[9]]
    if len(entries) != 3 or len(choices) != 4080 or len(dialogue) != 4080:
        raise ValueError('Unexpected capture duration or input count')
    report = {'scope': "Observed first-choice transitions and bounded native kitchen continuation; not complete window/startup fidelity", 'transitions': []}
    for r, frame in zip(entries, (2600, 2640, 2720)):
        c, d = choices[frame-1], dialogue[frame-1]
        expected = list(r[2:6])+[r[7]]+list(r[8:11])+list(r[12:15])
        actual = list(c[2:6])+[c[7]]+list(c[8:11])+[d[6], d[7], d[4]]
        if expected != actual:
            raise RuntimeError('Original/native choice transition mismatch')
        report['transitions'].append({'original_frame': r[0], 'native_frame': frame, 'exact_words': actual,
                                      'original_flags': hex(r[15]), 'native_flags': hex(d[2])})
    report['transition_word_order'] = ['fragment', 'cursor', 'choice_cursor', 'choice_result', 'armed', 'held', 'pressed', 'repeat', 'progress', 'advance', 'period']
    report['flags_limit'] = 'Observed flags differ by 0x404; this comparison does not establish flag equivalence. Native entry still uses diagnostic initialization. Input latch frames also vary between runs.'
    suffixes = ['', '.audio.pcm', '.music.bin', '.sound.bin', '.camera.bin', '.actor.bin', '.actor-9.bin', '.actor-1.bin', '.dialogue.bin', '.choices.bin']
    for variant in ('release', 'sanitize'):
        same_family(prefix, Path('local/native-choice-4080-'+variant+'-v1.png'), suffixes+['.json', '.audio.json'])
    same_family(Path('local/native-choice-2645-v1.png'), Path('local/native-choice-ui-2645-v1.png'), suffixes+['.json'])
    state = json.loads(Path(str(prefix)+'.json').read_text())
    if state['message_id'] != 10106 or state['text_progress'] != 1 or state['progression_flag_3'] != 1 or state['loaded_stream_banks'] != 0:
        raise RuntimeError('Native continuation state changed')
    accepted = [(r[0], r[1], choices[i-1][4]) for i, r in enumerate(choices)
                if i and r[2] == 0 and choices[i-1][6] and r[9] & 0x40]
    if accepted != [(2720, 10102, 1), (3640, 10105, 2)]:
        raise RuntimeError('Native selected branch sequence changed')
    for name, selection in (('zero', 0), ('two', 2)):
        other = Path('local/native-choice-'+name+'-3300-v1.png')
        trace = rows(Path(str(other)+'.choices.bin'), 12)
        if trace[2719][5] != selection or json.loads(Path(str(other)+'.json').read_text())['message_id'] != 10104:
            raise RuntimeError('Alternate first choice did not reach expected continuation')
    report['alternate_first_answers'] = [0, 2]
    camera = rows(Path(str(prefix)+'.camera.bin'), 16)
    if camera[-1][1] != 0xfb8:
        raise RuntimeError('Unexpected final event PC')
    report['continuation'] = {'frames': 4080, 'message': 10106, 'event_pc': '0xfb8', 'progress': 1, 'first_answer': 1, 'second_answer': 2,
                              'all_three_builds_exact': True, 'capture': str(prefix), 'next_unimplemented_opcode': '0x5e', 'next_event_pc': '0xfbc'}
    audio = json.loads(Path('local/native-choice-ui-2645-v1.png.audio.json').read_text())
    report['appkit'] = {'frames': 2645, 'headless_image_pcm_state_and_traces_exact': True, 'physical_input_or_listening_verified': False,
                       'device_frames_enqueued': audio['device_frames_enqueued'], 'device_underrun_frames': audio['device_underrun_frames'],
                       'capture': 'local/native-choice-ui-2645-v1.png'}
    report['reference'] = 'local/reference-choice-select-v1'
    paths = [prefix, Path(str(prefix)+'.choices.bin'), Path(str(prefix)+'.dialogue.bin'), reference_path]
    report['hashes'] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
    write_exact(args.output, (json.dumps(report, indent=2)+'\n').encode())
    print('Three original choice transitions, AppKit replay and three-build native continuation agree within the stated scope')

if __name__ == '__main__':
    main()
