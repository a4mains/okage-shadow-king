#!/usr/bin/env python3
"""Verify owned UI construction plus the first neutral 11fec8 dispatcher call."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

import verify_battle_interface as interface
import verify_ui_binding as binding
import verify_ui_window as window

Invalid, need, u32 = window.Invalid, window.need, window.u32
NATIVE_SCHEMA = 'okage-native-ui-first-update-v1'
NATIVE_SCOPE = ('owned constructors and first 11fec8 invocation; neutral native input; '
                'no second dispatcher, resource clock, layout or rendering')
OUTPUT_SCHEMA = 'okage-ui-first-update-verification-v1'
MANAGER_BYTES = 0xef0
MANAGER_WORDS = MANAGER_BYTES // 4
CALLBACKS = (0x11fec8, 0x120ad0, 0x120b48)
MANAGER_NORMALIZED = (0, 4, 8, 12, 16, 20, 24, 0x44, 0x94, 0x9c,
                      0x114, 0x124, 0x174, 0x17c, 0x1f4)
NEUTRAL_INPUT = {'held': 0, 'pressed': 0, 'released': 0, 'repeat': 0,
                 'left_x': 128, 'left_y': 128, 'display_rate': 60}

def strict_parse(text):
    return interface.parse(text)

def load_reference(path):
    return binding.load_reference(path)

def _manager_bytes(row, root, manager, label):
    return interface.raw(row, 'manager_raw', MANAGER_BYTES, manager)

def _manager_words(values, label):
    need(isinstance(values, list) and len(values) == MANAGER_WORDS,
         f'{label}: expected {MANAGER_WORDS} words')
    return [u32(v, f'{label}[{i}]') for i, v in enumerate(values)]

def _word(data, offset):
    return struct.unpack_from('<I', data, offset)[0]

def _validate_original_manager(reference):
    _header, rows, _finish = reference
    entry, complete = rows[0], rows[1]
    root = u32(entry.get('root'), 'entry.root')
    manager = u32(entry.get('manager'), 'entry.manager')
    need(root and manager and root != manager and complete.get('manager') == manager,
         'first dispatcher manager/root identity changed')
    roots = [interface.raw(r, 'battle_root', 0x81d0, root) for r in (entry, complete)]
    children = [tuple(_word(data, at) for at in (0x8064, 0x8068, 0x806c, 0x8070))
                for data in roots]
    need(children[0] == children[1] and len(set(children[0])) == 4 and all(children[0]),
         'original child owners changed or overlap')
    need(root not in children[0] and manager not in children[0],
         'original manager/root overlaps child owners')
    need(all(_word(data, 0x8060) == manager for data in roots),
         'original root does not retain the same manager')

    authored = interface.raw(complete, 'authored_interface_records_and_terminator', 0xe4, 0x248100)
    source_texture = u32(complete.get('shared_globals', {}).get('battle_ui_texture'),
                         'complete.battle_ui_texture')
    need(source_texture != 0xffffffff and source_texture ==
         u32(entry.get('shared_globals', {}).get('battle_ui_texture'), 'entry.battle_ui_texture'),
         'source texture token changed')
    originals = []
    for row, post in ((entry, False), (complete, True)):
        raw = _manager_bytes(row, root, manager, 'manager')
        need(tuple(_word(raw, off) for off in (0, 4, 8)) == CALLBACKS,
             'original manager callback table mismatch')
        need(_word(raw, 0xc) == children[0][3] and _word(raw, 0x10) == 0 and
             _word(raw, 0x14) == root and _word(raw, 0x18) == 0,
             'original manager sibling/parent/child links mismatch')
        need(_word(raw, 0x1c) == (_word(roots[0], 0x1c) & 0x18000000),
             'original inherited manager flags mismatch')
        need(tuple(_word(raw, off) for off in (0x114, 0x1f4)) ==
             ((manager, manager) if post else (0, 0)),
             'original layer parent backlinks do not match callback phase')
        for index, stream_tuple in enumerate(window.STREAMS):
            stream, _a, _b, _typ, callback = stream_tuple
            base = 0x40 + index * 0xe0
            need(_word(raw, base + 4) == source_texture,
                 f'original layer{index} texture identity mismatch')
            need(_word(raw, base + 0x54) == stream and _word(raw, base + 0x5c) == callback,
                 f'original layer{index} stream/callback identity mismatch')
            need(_word(raw, base + 0x50) & 0x40 and
                 _word(raw, base + 0x60) == stream_tuple[3],
                 f'original layer{index} state/type identity mismatch')
        for index, (stream, _a, _b, layer_type, callback) in enumerate(window.STREAMS):
            base = index * 112
            need(_word(authored, base) == stream and _word(authored, base + 0x54) == 0x1fdba8 and
                 _word(authored, base + 0x58) == (0x163fb8, 0x168228)[index] and
                 _word(authored, base + 0x5c) == callback and _word(authored, base + 0x60) == layer_type,
                 f'authored interface row {index} reference mismatch')
        originals.append(raw)
    return root, manager, children[0], source_texture, originals

def _manager_differences(reference, native):
    root, manager, children, texture, originals = _validate_original_manager(reference)
    before = _manager_words(native.get('manager_before'), 'native.manager_before')
    after = _manager_words(native.get('manager_after'), 'native.manager_after')
    source_callback = {0: CALLBACKS[0], 4: CALLBACKS[1], 8: CALLBACKS[2]}
    binding_values = {
        0xc: (children[3], 0, 'previous-sibling enemy owner'),
        0x10: (0, 0, 'null next sibling'),
        0x14: (root, 0, 'parent battle root'),
        0x18: (0, 0, 'null child link'),
        0x44: (texture, 1, 'shared texture binding ID'),
        0x94: (window.STREAMS[0][0], 1, 'type2 stream binding ID'),
        0x9c: (window.STREAMS[0][4], 1, 'type2 callback binding ID'),
        0x124: (texture, 1, 'shared texture binding ID'),
        0x174: (window.STREAMS[1][0], 2, 'type3 stream binding ID'),
        0x17c: (window.STREAMS[1][4], 2, 'type3 callback binding ID'),
    }
    for offset, callback in source_callback.items():
        binding_values[offset] = (callback, 0, 'source callback identity/native dispatcher')

    differences, normalizations = [], []
    compared = 0
    for phase, src, got in (('before', originals[0], before), ('after', originals[1], after)):
        for offset in range(0, MANAGER_BYTES, 4):
            expected_source = _word(src, offset)
            actual = got[offset // 4]
            if offset in binding_values:
                source_expected, native_expected, role = binding_values[offset]
                need(expected_source == source_expected,
                     f'original {phase} {role} mismatch at {offset:#x}')
                if actual != native_expected:
                    differences.append({'component': 'manager', 'phase': phase,
                                        'offset': offset, 'expected': native_expected, 'actual': actual})
                normalizations.append({'phase': phase, 'offset': offset, 'role': role,
                                       'original': source_expected, 'native': native_expected})
                continue
            if offset in (0x114, 0x1f4):
                expected_source = manager if phase == 'after' else 0
                native_expected = 1 if phase == 'after' else 0
                need(_word(src, offset) == expected_source,
                     f'original {phase} manager backlink mismatch at {offset:#x}')
                if actual != native_expected:
                    differences.append({'component': 'manager', 'phase': phase,
                                        'offset': offset, 'expected': native_expected, 'actual': actual})
                normalizations.append({'phase': phase, 'offset': offset,
                                       'role': 'typed manager parent link',
                                       'original': expected_source, 'native': native_expected})
                continue
            compared += 1
            if expected_source != actual:
                differences.append({'component': 'manager', 'phase': phase,
                                    'offset': offset, 'expected': expected_source, 'actual': actual})
    need(set(MANAGER_NORMALIZED) == set(binding_values) | {0x114, 0x1f4},
         'manager normalization coverage mismatch')
    return differences, normalizations, compared, {'root': root, 'manager': manager,
        'enemy_owner': children[3], 'source_texture': texture}

def _compare_windows(reference_row, native_windows, label, after=False):
    need(isinstance(native_windows, list) and len(native_windows) == 2,
         f'native {label} requires two windows')
    differences, compared, per_slot = [], 0, []
    for slot, native_values in enumerate(native_windows):
        if after:
            need(isinstance(native_values, dict) and native_values.get('slot') == slot,
                 f'native post-window {slot} slot identity mismatch')
            native_values = native_values.get('scalar_words')
        need(isinstance(native_values, list) and len(native_values) == window.WINDOW_WORDS,
             f'native {label}[{slot}] extent mismatch')
        original_entry = reference_row['controller_windows'][slot]
        source = binding.raw(original_entry['raw'], f'original {label} window',
                             window.WINDOW_BYTES, original_entry['pointer'])
        pointer_words = set(window.EXCLUDED_WORDS)
        if after:
            cursor = _word(source, 0x2edc)
            need(cursor <= window.WINDOW_WORDS and cursor % 2 == 0,
                 f'original post-window {slot} embedded cursor is invalid')
            # The binding verifier separately validates every opcode/referent;
            # these are precisely the argument words holding original pointers.
            pointer_words.update((0x26dc // 4) + i for i in range(1, cursor, 2))
        slot_count = 0
        for index, value in enumerate(native_values):
            offset = index * 4
            if index in pointer_words:
                need(value is None, f'native {label} pointer/unknown lane {slot}:{offset:#x} must be null')
                continue
            actual = u32(value, f'native {label}[{slot}][{index}]')
            expected = _word(source, offset)
            slot_count += 1; compared += 1
            if expected != actual:
                differences.append({'component': label, 'slot': slot, 'offset': offset,
                                    'expected': expected, 'actual': actual})
        per_slot.append(slot_count)
    return differences, compared, per_slot

def compare(reference, native):
    _header, rows, _finish = reference
    entry, complete = rows[0], rows[1]
    need(isinstance(native, dict) and set(native) == {
        'schema', 'scope', 'windows', 'hud', 'manager_before', 'manager_after',
        'windows_before', 'resource_accumulators', 'input'},
        'native first-update diagnostic fields mismatch')
    need(native.get('schema') == NATIVE_SCHEMA and native.get('scope') == NATIVE_SCOPE,
         'native first-update schema/scope mismatch')

    manager_diffs, manager_norms, manager_compared, identity = _manager_differences(reference, native)
    before_windows = native.get('windows_before')
    need(isinstance(before_windows, list) and len(before_windows) == 2,
         'native requires two pre-update windows')
    window_diffs, window_compared, before_per_slot = _compare_windows(
        entry, before_windows, 'window_before')

    accumulator = native.get('resource_accumulators')
    need(isinstance(accumulator, list) and len(accumulator) == 16,
         'native resource accumulator requires 16 words')
    accumulator = [u32(v, f'resource_accumulators[{i}]') for i, v in enumerate(accumulator)]
    original_accumulators = []
    for row, label in ((entry, 'entry'), (complete, 'complete')):
        raw_acc = binding.raw(row.get('type2_widget_index_accumulators'),
                              f'{label} 200a88 owner', 64, 0x200a88)
        original_accumulators.append(struct.unpack('<16I', raw_acc))
    need(not any(original_accumulators[0]),
         'original pre-callback 200a88 owner is not the required zero initial state')
    accumulator_diffs = []
    for i, (expected, actual) in enumerate(zip(original_accumulators[1], accumulator)):
        if expected != actual:
            accumulator_diffs.append({'word': i, 'expected': expected, 'actual': actual})
    if original_accumulators[0] != original_accumulators[1]:
        accumulator_diffs.append({'field': 'original_pre_post_owner_changed',
                                  'before': list(original_accumulators[0]),
                                  'after': list(original_accumulators[1])})

    native_input = native.get('input')
    need(isinstance(native_input, dict) and set(native_input) == set(NEUTRAL_INPUT),
         'native input must serialize exactly the neutral dispatcher fields')
    for key, expected in NEUTRAL_INPUT.items():
        need(u32(native_input.get(key), f'input.{key}') == expected,
             f'native first-update input {key} is not neutral')
    for row, label in ((entry, 'entry'), (complete, 'complete')):
        pad = row.get('pad_globals')
        need(isinstance(pad, dict) and pad.get('0x33958c') == 0 and pad.get('0x339590') == 0,
             f'original {label} packed pad latches are not neutral')

    projected = {'schema': binding.SCHEMA, 'scope': binding.SCOPE,
                 'windows': native['windows'], 'hud': native['hud']}
    binding_report = binding.compare(reference, projected)
    after_diffs, after_compared, after_per_slot = _compare_windows(
        complete, native['windows'], 'window_after', after=True)
    binding_report['scope'] = (
        'reused binding-only assertions on RUW stream, reset, glyph and HUD effects; '
        'full callback comparison is in the enclosing report')
    differences = manager_diffs + window_diffs + after_diffs + accumulator_diffs + binding_report['differences']
    return {
        'schema': OUTPUT_SCHEMA, 'status': 'fail' if differences else 'pass',
        'scope': NATIVE_SCOPE,
        'reference_frame_pair': [entry['frame'], complete['frame']],
        'native_identity': identity,
        'manager': {'total_words_per_phase': MANAGER_WORDS,
                    'compared_words': manager_compared,
                    'normalized_words': len(manager_norms), 'normalizations': manager_norms},
        'windows_before': {'compared_words': window_compared,
                           'per_slot': before_per_slot,
                           'excluded_offsets_hex': [hex(x) for x in window.EXCLUDED_OFFSETS]},
        'windows_after': {'compared_words': after_compared, 'per_slot': after_per_slot,
                          'excluded_offsets_hex': [hex(x) for x in window.EXCLUDED_OFFSETS],
                          'embedded_pointer_argument_words_excluded': True},
        'resource_accumulators': {'compared_words': 16,
                                  'original_before': list(original_accumulators[0]),
                                  'original_after': list(original_accumulators[1]),
                                  'native_after': accumulator},
        'neutral_input': NEUTRAL_INPUT,
        'reused_binding_only_checks': binding_report,
        'difference_count': len(differences),
        'first_difference': differences[0] if differences else None,
        'differences': differences,
        'limitations': [
            'The comparison covers construction plus the first neutral 11fec8 dispatcher invocation only; no second dispatcher, resource clock, layout, or rendering is included.',
            'Stick/analog and other controller globals are not captured at the original boundary; only the two packed pad latches and the explicitly serialized native neutral input are validated.',
            'The native resource-accumulator diagnostic serializes only the callback result; original pre-state is asserted all-zero, and the native owner is known to default-zero at construction, but a distinct native pre-callback snapshot is not serialized.',
            'Manager callback, owner, texture, stream, and manager-backlink pointers are checked against original source relationships, then mapped to native zero placeholders or typed IDs as listed.',
            'Native variables are constructor-owned input, not original RAM; variable/global equivalence is not claimed.',
            'The reused HUD check covers only captured HUD construction/widget effects; full HUD state is not compared.',
            'No UI activation, later callback progression, first-frame image, or full battle interface fidelity is claimed.'
        ]
    }

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', required=True)
    parser.add_argument('--native', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args(argv)
    output = Path(args.output)
    try:
        need(not output.exists() and not output.is_symlink(), 'refusing existing output')
        inputs = [Path(args.reference), Path(args.native)]
        need(output.resolve() not in {p.resolve() for p in inputs}, 'output aliases an input')
        reference = load_reference(args.reference)
        native = strict_parse(Path(args.native).read_text())
        report = compare(reference, native)
        report['inputs'] = {name: {'path': str(path.resolve()), 'sha256': sha(path)}
                            for name, path in zip(('original', 'native'), inputs)}
        report['verifier_sha256'] = {Path(m.__file__).name: sha(m.__file__)
                                     for m in (binding, binding.hud, window, interface)}
        report['verifier_sha256'][Path(__file__).name] = sha(__file__)
        with output.open('x') as f:
            json.dump(report, f, indent=2); f.write('\n')
    except (Invalid, OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        print(f'verify-ui-first-update: {exc}', file=sys.stderr)
        return 2
    print(f"{report['status']}: {report['difference_count']} differences; "
          f"{report['windows_before']['compared_words']} pre-window words")
    return 0 if report['status'] == 'pass' else 1

if __name__ == '__main__':
    raise SystemExit(main())
