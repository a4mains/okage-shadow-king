#!/usr/bin/env python3
"""Audit event69/message10002 opening and actor-target camera integration.

Checks 90 controller returns/pressed latches,180 field-manager samples,
90 mode/blend states and60 settled target vectors. Initial movement and camera
position differ; this is not whole-camera, raster, audio or conversation fidelity.
"""
import argparse
import json
from pathlib import Path
import struct
from prepare_assets import ELF_HASH
from verify_event import digest, write_exact
from verify_message_opening import require, rows, compare_controller
from verify_interaction_scene import actor_rows

HASHES = {
    '.controller.bin': '226658a520038c9529bc85bd6e140863246e557142e09f3039529a7bf44a7ab6',
    '.camera.bin': 'bddee16c9eb953ab0a4d8ce090c1269eee768f412b964a18c4b0ed33810f014c',
    '.interaction.bin': '1e3550b68713bcdeafc5231c4466b286568a7c8ee8720fb1fc3d215c0e9d78b9',
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--answer', action='store_true', help='Also audit the completed original480 first-answer continuation')
    parser.add_argument('--complete', action='store_true', help='Check native-only conversation closure and subsequent road movement')
    parser.add_argument('--ending', action='store_true', help='Audit original720 ending with observed input alignment')
    prefix_check = parser.add_mutually_exclusive_group()
    prefix_check.add_argument('--original-only', action='store_true',
        help='Run original-game comparisons without claiming an unchanged earlier native prefix')
    prefix_check.add_argument('--prior-capture', type=Path,
        default=Path('local/native-gate-message-close-8260-system-native-v1.png'),
        help='Reviewed native8260 baseline; this regression check is separate from the original comparisons')
    args = parser.parse_args()
    original = 'local/reference-boys-opening-180-v1'
    ref = lambda suffix: Path(original + suffix)
    native = lambda suffix: Path(str(args.capture) + suffix)
    manifest = Path(str(args.capture).removesuffix('.png') + '.run.json')
    run = json.loads(manifest.read_text())
    original_run = json.loads(ref('.run.json').read_text())
    require(run['exit_code'] == original_run['exit_code'] == 0, 'Incomplete opening run')
    require(digest(Path('local/extracted/SCUS_971.29')) == ELF_HASH, 'Wrong retail ELF')
    files = [manifest, ref('.run.json'), args.capture]
    for suffix, expected in HASHES.items():
        require(digest(ref(suffix)) == expected, 'Changed opening reference: ' + suffix)
        files.append(ref(suffix))
    interaction = rows(native('.interaction.bin').read_bytes(), 8)
    camera = rows(native('.camera.bin').read_bytes(), 16)
    state = rows(native('.camera-state.bin').read_bytes(), 5)
    total = len(interaction)
    require(total >= 9840 and len(camera) == len(state) == total, 'Native trace extent')
    for trace in [interaction, camera, state]:
        require([r[0] for r in trace] == list(range(1, total + 1)), 'Native frame identity')
    words = compare_controller(ref('.controller.bin').read_bytes(), native('.dialogue.bin').read_bytes(),
                               first_original=90, first_native=9751, frames=90, total=total, message_id=10002)
    controller = rows(ref('.controller.bin').read_bytes(), 64)
    for r in controller[1::2]:
        require(interaction[9660 + r[0]][1] == r[9], 'Pressed input differs')
    observed = rows(ref('.interaction.bin').read_bytes(), 176)
    require([(r[0], r[3]) for r in observed if r[1] == 0x1bb7e0] == [(90, 69)], 'Original dispatch differs')
    require(interaction[9750][2:6] == (0xffffffed, 69, 1, 1), 'Native dispatch differs')
    manager = [r for r in observed if r[1] == 0x133a98]
    require([r[0] for r in manager] == list(range(180)), 'Manager extent')
    for r in manager:
        n = interaction[9659 + r[0]]
        require((n[4], n[7], n[5], n[6]) == (int(bool(r[20] & 0x100)), r[21], r[15], r[150]),
                'Manager differs at ' + str(r[0]))
    reference_camera = rows(ref('.camera.bin').read_bytes(), 32)
    require([r[0] for r in reference_camera] == list(range(180)), 'Camera extent')
    for r in reference_camera[90:]:
        n = state[9660 + r[0]]
        require((n[1], n[3], n[4]) == (r[2], r[3], r[28]) and r[2] == 3, 'Camera mode/blend differs')
    for r in reference_camera[120:]:
        # Reference target is raw; compare rendered target only after blend completion.
        require(camera[9660 + r[0]][8:12] == r[12:16], 'Settled actor target differs')
    for suffix in ([] if args.original_only else ['.dialogue.bin', '.choices.bin', '.interaction.bin', '.movement.bin', '.camera.bin', '.actor-1.bin']):
        before = Path(str(args.prior_capture) + suffix)
        width = {'.dialogue.bin':12, '.choices.bin':12, '.interaction.bin':8,
                 '.movement.bin':16, '.camera.bin':16}.get(suffix)
        baseline = rows(before.read_bytes(),width) if width else actor_rows(before)
        require([r[0] for r in baseline] == list(range(1,8261)), 'Prior baseline must contain8260 complete frames')
        require(native(suffix).read_bytes().startswith(before.read_bytes()), 'Prior8260 route changed: ' + suffix)
        files.extend([before, native(suffix)])
    files.append(native('.camera-state.bin'))
    files.extend(native(suffix) for suffix in ('.interaction.bin', '.camera.bin', '.dialogue.bin'))
    if args.answer or args.ending:
        files.append(native('.choices.bin'))
    answer = None
    if args.answer:
        require(total >= 10320, 'First-answer capture is too short')
        prefix = 'local/reference-boys-answer-480-v1'
        reference = lambda suffix: Path(prefix + suffix)
        hashes = {
            '.controller.bin': '8913e17298b56fa8ef4c3e6976c9701d6aae38f4b26526fd19f82208d8b75f23',
            '.interaction.bin': 'f2b267836a6d25d4ec126e0c1500b15041357ee1dd2f986a0c6d5743533789e0',
            '.camera.bin': '45a89b41f8886c6db7d88112893528d34512d38d3b7cb5ee8dd6aacfbbce3cb4',
            '.controller.bin.choices.bin': '19b70566e4ce5d83660892e5d82d170494288fa6864b71679e1901a144f4dd29',
        }
        answer_run = json.loads(reference('.run.json').read_text())
        require(answer_run['exit_code'] == 0, 'Incomplete original answer')
        for suffix, expected in hashes.items():
            require(digest(reference(suffix)) == expected, 'Changed answer reference')
            files.append(reference(suffix))
        files.append(reference('.run.json'))
        count = compare_controller(reference('.controller.bin').read_bytes(), native('.dialogue.bin').read_bytes(),
                                   first_original=0, first_native=9841, frames=480, total=total, message_id=10002)
        for r in rows(reference('.controller.bin').read_bytes(), 64)[1::2]:
            require(interaction[9840 + r[0]][1] == r[9], 'Answer pressed latch differs')
        managers = [r for r in rows(reference('.interaction.bin').read_bytes(), 176) if r[1] == 0x133a98]
        require([r[0] for r in managers] == list(range(480)), 'Answer manager extent')
        for r in managers:
            n = interaction[9839 + r[0]]
            require((n[4], n[7], n[5], n[6]) == (int(bool(r[20] & 0x100)), r[21], r[15], r[150]),
                    'Answer manager differs')
        cameras = rows(reference('.camera.bin').read_bytes(), 32)
        require([r[0] for r in cameras] == list(range(480)), 'Answer camera extent')
        for r in cameras:
            n = state[9840 + r[0]]
            require((n[1], n[3], n[4]) == (r[2], r[3], r[28]), 'Answer camera mode/blend differs')
        choices = rows(reference('.controller.bin.choices.bin').read_bytes(), 16)
        native_choices = rows(native('.choices.bin').read_bytes(), 12)
        require(len(choices) == 960 and len(native_choices) == total, 'Answer choice extent')
        for r in choices[1::2]:
            require(r[2:6] == native_choices[9840 + r[0]][2:6], 'Answer choice state differs')
        answer = dict(original_run=answer_run, controller_words=count, pressed_latches=480,
                      manager_words=1920, camera_mode_blend_words=1440, choice_words=1920)
    ending = None
    if args.ending:
        require(total >= 11040, 'Ending capture is too short')
        prefix = 'local/reference-boys-close-720-v1'
        reference = lambda suffix: Path(prefix + suffix)
        hashes = {
            '.controller.bin': 'a174b3ba0333c3ab9cca22b06661f243fd52558eb7cdc1f6044691570c8a0f6d',
            '.interaction.bin': '83b50a1d8bd1c71ac4600ef27384c948babf92dbb72312a983f3be8b21289db1',
            '.camera.bin': '91ae4dccdba9d8a8539d48f092a42e645166a8fa6b4bcfdbbf976b0e797d8399',
            '.controller.bin.choices.bin': 'a86f99c7a5718d3d0078ce65151763aee2ba8ebbb2d8752fa66861373b27eb1e',
        }
        ending_run = json.loads(reference('.run.json').read_text())
        require(ending_run['exit_code'] == 0, 'Incomplete original ending')
        for suffix, expected in hashes.items():
            require(digest(reference(suffix)) == expected, 'Changed ending reference')
            files.append(reference(suffix))
        files.append(reference('.run.json'))
        count = compare_controller(reference('.controller.bin').read_bytes(), native('.dialogue.bin').read_bytes(),
                                   first_original=0, first_native=10321, frames=691, total=total, message_id=10002)
        for r in rows(reference('.controller.bin').read_bytes(), 64)[1::2]:
            require(interaction[10320 + r[0]][1] == r[9], 'Ending pressed latch differs')
        managers = [r for r in rows(reference('.interaction.bin').read_bytes(), 176) if r[1] == 0x133a98]
        require([r[0] for r in managers] == list(range(720)), 'Ending manager extent')
        for r in managers:
            n = interaction[10319 + r[0]]
            require((n[4], n[7], n[5], n[6]) == (int(bool(r[20] & 0x100)), r[21], r[15], r[150]),
                    'Ending manager differs at ' + str(r[0]))
        cameras = rows(reference('.camera.bin').read_bytes(), 32)
        require([r[0] for r in cameras] == list(range(720)), 'Ending camera extent')
        for r in cameras:
            n = state[10320 + r[0]]
            require((n[1], n[3], n[4]) == (r[2], r[3], r[28]), 'Ending camera mode/blend differs')
        choices = rows(reference('.controller.bin.choices.bin').read_bytes(), 16)
        native_choices = rows(native('.choices.bin').read_bytes(), 12)
        require(len(choices) == 1382 and len(native_choices) == total, 'Ending choice extent')
        for r in choices[1::2]:
            require(r[2:6] == native_choices[10320 + r[0]][2:6], 'Ending choice state differs')
        ending = dict(original_run=ending_run, controller_words=count, pressed_latches=691,
                      manager_words=2880, camera_mode_blend_words=2160, choice_words=2764,
                      scope='Ending controller, choices, manager and camera mode/blend; no movement/raster/audio equivalence')
    floats = lambda words: list(struct.unpack('<4f', struct.pack('<4I', *words)))
    report = dict(scope=__doc__, native_run=run, original_run=original_run, event_frame=9751,
                  controller_words=words, pressed_latches=90, manager_words=720,
                  camera_mode_blend_words=270, settled_actor_target_words=240,
                  unchanged_prefix_frames=None if args.original_only else 8260,
                  native_prefix_compared=not args.original_only,
                  camera_endpoint={'native_position': floats(camera[9839][4:8]),
                                   'original_position': floats(reference_camera[-1][4:8]),
                                   'position_equivalence_claimed': False},
                  sha256={str(p): digest(p) for p in files})
    report['first_answer'] = answer
    report['ending'] = ending
    if args.complete:
        require(total == 11309, 'Native completion route extent')
        dialogue = rows(native('.dialogue.bin').read_bytes(), 12)
        choices = rows(native('.choices.bin').read_bytes(), 12)
        accepts = [(r[0], r[5]) for i, r in enumerate(choices[9750:], 9750)
                   if choices[i - 1][7] and not r[7]]
        require(accepts == ([(10031, 0), (10571, 0), (10892, 0)] if args.ending else
                            [(10031, 0), (10570, 0), (10890, 0)]), 'Native answers differ')
        shift = 2 if args.ending else 0
        require(dialogue[11007 + shift][10] == 9 and interaction[11011 + shift][4:7] == (0, 1, 1)
                and interaction[11012 + shift][4:7] == (0, 0, 1), 'Native closure/handoff differs')
        variables = native('.variables.bin').read_bytes()
        require(len(variables) == 4096 and variables[0x110] & 1, 'Native conversation flag absent')
        hero = actor_rows(native('.actor-1.bin'))
        require(len(hero) == total and hero[11039][12:16] != hero[-1][12:16], 'Native movement absent')
        require(all(r[4:7] == (0, 0, 1) for r in interaction[11012 + shift:]), 'Unexpected later event/control gate')
        report['native_completion'] = dict(scope='Original closure compared; subsequent movement native-only' if args.ending else 'Native execution only; original closure/movement not compared',
                                          answers=accepts, closed_stage_frame=11008 + shift, script_finished_frame=11012 + shift,
                                          control_return_frame=11013 + shift, flag='bank1:index0=1',
                                          final_hero_position=floats(hero[-1][12:16]))
        report['sha256'][str(native('.variables.bin'))] = digest(native('.variables.bin'))
    write_exact(args.output, (json.dumps(report, indent=2) + '\n').encode())
    print('Boys opening comparison passed; first answer:', bool(answer), 'ending:', bool(ending))

if __name__ == '__main__':
    main()
