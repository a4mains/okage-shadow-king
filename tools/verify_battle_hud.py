#!/usr/bin/env python3
"""Compare the bounded original/native battle HUD constructor effects."""
import argparse
import json
import struct
import sys
from pathlib import Path

from verify_ui_window import Invalid, load_json, load_reference, need, u32

REFERENCE_SCHEMA = 'okage-battle-ui-v1'
NATIVE_SCHEMA = 'okage-battle-hud-diagnostic-v1'
OUTPUT_SCHEMA = 'okage-battle-hud-verification-v1'
NATIVE_SCOPE = '163fb8 and 168228 RUW/stat side effects only; zero incoming party cache; no manager or callbacks'
GROUP_COUNTS = (21, 4, 56, 72, 46, 57)
GROUP_RECORD_OFFSETS = (0, 21, 25, 81, 153, 199)
GROUPS_OFFSET = 2188
RECORD_BYTES = 68
RUNTIME_BYTES = 44
RUNTIME_WORDS = 10
TEXT_BYTES = 128
TEXT_SOURCE_BYTES = 9344
RUNTIME_SOURCE_BYTES = 10560
RAW_SOURCE_BYTES = 19596
VARS_BYTES = 4096
GLOBAL_KEYS = ('type2_flag_a', 'type2_flag_b', 'type2_enabled', 'type3_ready',
               'type3_reset', 'type3_animation', 'type3_selection', 'ui_menu_state',
               'ui_mode_counter', 'ui_selection_pointer')

def _bytes(wrapper, label, expected_size, expected_address=None):
    need(isinstance(wrapper, dict), f'{label}: expected byte wrapper')
    text = wrapper.get('raw_hex')
    need(isinstance(text, str), f'{label}: missing raw_hex')
    try:
        data = bytes.fromhex(text)
    except ValueError as exc:
        raise Invalid(f'{label}: invalid hex: {exc}') from exc
    need(len(text) == 2 * len(data), f'{label}: noncanonical hex')
    need(wrapper.get('bytes') == len(data) == expected_size,
         f'{label}: expected {expected_size} bytes')
    if expected_address is not None:
        need(wrapper.get('address') == expected_address, f'{label}: source address mismatch')
    return data

def _words(data, label, count):
    need(len(data) == count * 4, f'{label}: wrong word extent')
    return struct.unpack('<' + 'I' * count, data)

def _original_widgets(complete):
    slot = complete.get('ruw_slot0')
    need(isinstance(slot, dict), 'original slot0 RUW owner is missing')
    need(set(slot) >= {'raw_pointer', 'runtime_pointer', 'text_pointer', 'metadata',
                       'template_group_counts', 'template_group_offsets',
                       'widget_group_counts', 'widget_group_offsets', 'raw_owner',
                       'runtime_owner', 'text_owner'}, 'original RUW slot metadata is incomplete')
    raw_pointer = u32(slot.get('raw_pointer'), 'original raw pointer')
    runtime_pointer = u32(slot.get('runtime_pointer'), 'original runtime pointer')
    text_pointer = u32(slot.get('text_pointer'), 'original text pointer')
    raw = _bytes(slot.get('raw_owner'), 'original raw owner', RAW_SOURCE_BYTES, raw_pointer)
    runtime = _bytes(slot.get('runtime_owner'), 'original runtime owner', RUNTIME_SOURCE_BYTES,
                     runtime_pointer)
    text = _bytes(slot.get('text_owner'), 'original text owner', TEXT_SOURCE_BYTES, text_pointer)

    metadata = _words(_bytes(slot.get('metadata'), 'original RUW metadata', 48), 'metadata', 12)
    need(metadata[:3] == (87, 1, 6), 'original RUW style/template/widget group counts mismatch')
    need(raw[:8] == b'rxuw1000', 'original RUW signature mismatch')
    need(slot['metadata'].get('address') == 0x200750,
         'original RUW metadata address mismatch')
    need(metadata[7:10] == (raw_pointer + 48, raw_pointer + 1440, raw_pointer + GROUPS_OFFSET),
         'original RUW table pointers do not match owned raw extent')
    expected_table_addresses = tuple(metadata[3:7])
    actual_table_addresses = tuple(slot[key].get('address') for key in
                                   ('template_group_counts', 'template_group_offsets',
                                    'widget_group_counts', 'widget_group_offsets'))
    need(actual_table_addresses == expected_table_addresses,
         'original RUW group table pointers do not match their captured owners')
    counts = _words(_bytes(slot.get('widget_group_counts'), 'widget group counts', 24),
                    'widget group counts', 6)
    offsets = _words(_bytes(slot.get('widget_group_offsets'), 'widget group offsets', 24),
                     'widget group offsets', 6)
    need(counts == GROUP_COUNTS and offsets == GROUP_RECORD_OFFSETS,
         'original RUW widget group layout mismatch')
    need(GROUPS_OFFSET + sum(counts) * RECORD_BYTES == len(raw),
         'original RUW widget table does not fill its bounded owner')

    expected_runtime_ptr = runtime_pointer
    expected_text_ptr = text_pointer
    result = []
    for group_index, count in enumerate(counts):
        widgets = []
        group_record_offset = GROUPS_OFFSET + offsets[group_index] * RECORD_BYTES
        for widget_index in range(count):
            record_offset = group_record_offset + widget_index * RECORD_BYTES
            words = struct.unpack_from('<17I', raw, record_offset)
            runtime_guest_pointer = words[16]
            widget_type, widget_flags = words[5], words[4]
            owns_runtime = widget_type == 1 or (widget_type <= 3 and (widget_flags & 0x40) != 0)
            need(bool(runtime_guest_pointer) == owns_runtime,
                 f'original widget {group_index}:{widget_index} runtime pointer presence is invalid')
            runtime_words = None
            text_bytes = None
            if runtime_guest_pointer:
                need(runtime_guest_pointer == expected_runtime_ptr,
                     f'original widget {group_index}:{widget_index} runtime pointer/order is invalid')
                runtime_offset = runtime_guest_pointer - runtime_pointer
                need(runtime_offset >= 0 and runtime_offset + RUNTIME_BYTES <= len(runtime),
                     f'original widget {group_index}:{widget_index} runtime pointer is out of bounds')
                runtime_words = struct.unpack_from('<11I', runtime, runtime_offset)
                expected_runtime_ptr += RUNTIME_BYTES
                if widget_type == 1:
                    text_guest_pointer = runtime_words[10]
                    need(text_guest_pointer == expected_text_ptr,
                         f'original widget {group_index}:{widget_index} text pointer/order is invalid')
                    text_offset = text_guest_pointer - text_pointer
                    need(text_offset >= 0 and text_offset + TEXT_BYTES <= len(text),
                         f'original widget {group_index}:{widget_index} text pointer is out of bounds')
                    text_bytes = text[text_offset:text_offset + TEXT_BYTES]
                    need(b'\0' in text_bytes,
                         f'original widget {group_index}:{widget_index} text has no terminator in owner')
                    expected_text_ptr += TEXT_BYTES
            widgets.append({'words': words, 'runtime': runtime_words,
                            'text': text_bytes, 'record_offset': record_offset})
        result.append(widgets)
    need(expected_runtime_ptr == runtime_pointer + len(runtime),
         'original runtime owner contains unreferenced or missing records')
    need(expected_text_ptr == text_pointer + len(text),
         'original text owner contains unreferenced or missing records')
    return result

def _nul_prefix(data, label):
    need(b'\0' in data, f'{label}: expected a NUL-terminated text extent')
    return data[:data.index(0) + 1]

def compare(reference, native):
    _header, entry, complete, _finish, root = reference
    need(isinstance(native, dict) and
         set(native) in ({'schema', 'scope', 'game_mode', 'selected', 'vars_hex', 'globals', 'groups'},
                         {'schema', 'scope', 'game_mode', 'selected', 'vars_hex', 'globals', 'groups', 'execution'}),
         'native diagnostic fields mismatch')
    need(native.get('schema') == NATIVE_SCHEMA, 'native diagnostic schema mismatch')
    need(native.get('scope') == NATIVE_SCOPE, 'native diagnostic scope mismatch')
    execution = native.get('execution', 'isolated')
    need(execution in ('isolated', 'native-session'), 'native execution must be isolated or native-session')
    need(u32(native.get('game_mode'), 'native game_mode') == 3 and
         u32(native.get('selected'), 'native selected') == 0,
         'native HUD input parameters mismatch')

    vars_hex = native.get('vars_hex')
    need(isinstance(vars_hex, str), 'native vars_hex is missing')
    try:
        variables = bytes.fromhex(vars_hex)
    except ValueError as exc:
        raise Invalid(f'native vars_hex is invalid: {exc}') from exc
    need(len(vars_hex) == len(variables) * 2 and len(variables) == VARS_BYTES,
         f'native vars_hex must contain exactly {VARS_BYTES} bytes')

    original_globals = complete.get('shared_globals')
    native_globals = native.get('globals')
    need(isinstance(original_globals, dict), 'original shared globals are missing')
    need(isinstance(native_globals, dict) and set(native_globals) == set(GLOBAL_KEYS),
         'native globals must contain exactly the reviewed scalar keys')
    differences = []
    for key in GLOBAL_KEYS:
        expected = u32(original_globals.get(key), f'original global {key}')
        actual = u32(native_globals.get(key), f'native global {key}')
        if expected != actual:
            differences.append({'field': f'globals.{key}', 'expected': expected, 'actual': actual})

    original_groups = _original_widgets(complete)
    native_groups = native.get('groups')
    need(isinstance(native_groups, list) and len(native_groups) == len(GROUP_COUNTS),
         'native must contain exactly six widget groups')
    compared_words = compared_runtime_words = compared_text_bytes = 0
    widget_count = 0
    group_reports = []
    for gi, expected_widgets in enumerate(original_groups):
        actual_widgets = native_groups[gi]
        need(isinstance(actual_widgets, list) and len(actual_widgets) == GROUP_COUNTS[gi],
             f'native group {gi} widget count mismatch')
        group_words = group_runtime = group_text = 0
        for wi, original in enumerate(expected_widgets):
            actual = actual_widgets[wi]
            label = f'native widget {gi}:{wi}'
            need(isinstance(actual, dict) and set(actual) == {'words', 'runtime', 'text_hex'},
                 f'{label}: fields mismatch')
            words = actual.get('words')
            need(isinstance(words, list) and len(words) == 17,
                 f'{label}: expected 17 raw words')
            for word_index, expected in enumerate(original['words']):
                got = u32(words[word_index], f'{label}.words[{word_index}]')
                if word_index == 16:
                    need(got == 0,
                         f'{label}.words[16]: native pointer-free HUD data must keep this word zero')
                    continue
                compared_words += 1
                group_words += 1
                if got != expected:
                    differences.append({'field': f'groups[{gi}][{wi}].words[{word_index}]',
                                        'expected': expected, 'actual': got})
            if original['runtime'] is None:
                need(actual['runtime'] is None,
                     f'{label}: native runtime should be absent for original pointer-null widget')
            else:
                runtime = actual.get('runtime')
                need(isinstance(runtime, list) and len(runtime) == RUNTIME_WORDS,
                     f'{label}: expected {RUNTIME_WORDS} runtime words')
                for ri, expected in enumerate(original['runtime'][:RUNTIME_WORDS]):
                    got = u32(runtime[ri], f'{label}.runtime[{ri}]')
                    compared_runtime_words += 1
                    group_runtime += 1
                    if got != expected:
                        differences.append({'field': f'groups[{gi}][{wi}].runtime[{ri}]',
                                            'expected': expected, 'actual': got})
            if original['text'] is None:
                need(actual.get('text_hex') is None,
                     f'{label}: native text should be absent for original text-pointer-null widget')
            else:
                text_hex = actual.get('text_hex')
                need(isinstance(text_hex, str) and len(text_hex) == TEXT_BYTES * 2,
                     f'{label}: expected {TEXT_BYTES}-byte text_hex')
                try:
                    actual_text = bytes.fromhex(text_hex)
                except ValueError as exc:
                    raise Invalid(f'{label}: invalid text_hex: {exc}') from exc
                need(len(actual_text) == TEXT_BYTES, f'{label}: text extent mismatch')
                expected_prefix = _nul_prefix(original['text'], f'original {label} text')
                actual_prefix = _nul_prefix(actual_text, label)
                compared_text_bytes += len(expected_prefix)
                group_text += len(expected_prefix)
                if expected_prefix != actual_prefix:
                    differences.append({'field': f'groups[{gi}][{wi}].text',
                                        'expected_hex': expected_prefix.hex(),
                                        'actual_hex': actual_prefix.hex()})
            widget_count += 1
        group_reports.append({'group': gi, 'widgets': len(expected_widgets),
                              'compared_raw_words': group_words,
                              'compared_runtime_words': group_runtime,
                              'compared_text_bytes': group_text})

    return {
        'schema': OUTPUT_SCHEMA,
        'status': 'pass' if not differences else 'fail',
        'reference_schema': REFERENCE_SCHEMA,
        'native_schema': NATIVE_SCHEMA,
        'reference_frame': u32(entry.get('frame'), 'reference entry frame'),
        'reference_root': root,
        'native_execution': execution,
        'widget_groups': group_reports,
        'widget_count': widget_count,
        'compared_raw_words': compared_words,
        'compared_runtime_words': compared_runtime_words,
        'compared_text_bytes': compared_text_bytes,
        'compared_global_scalars': len(GLOBAL_KEYS),
        'native_variables_bytes_validated_but_not_compared': len(variables),
        'first_difference': differences[0] if differences else None,
        'difference_count': len(differences),
        'differences': differences,
        'unsupported': [
            'Native variables extent and encoding are validated, but original variables were not captured and no variable equality is claimed.',
            'Original raw word16 is a guest pointer and is excluded from equality; the corresponding native pointer-free word is required to remain zero.',
            'Original runtime word10 is treated as a text pointer only for type-1 widgets. For other widget types it is an uninitialized or unmodeled scalar and is not interpreted or compared.',
            'Runtime comparison covers the first ten words of each observed 44-byte original record. The final word is compared as a text pointer only for type-1 widgets; its other-type value is excluded.',
            'This verifies RUW/stat side effects in the declared scope only; it makes no claim about manager allocation, callbacks, rendering, or full UI behavior.'
        ]
    }

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', required=True, help='natural UI observer JSONL')
    parser.add_argument('--native', required=True, help='native HUD diagnostic JSON')
    parser.add_argument('--output', required=True, help='fresh verification JSON path')
    args = parser.parse_args(argv)
    output = Path(args.output)
    if output.exists() or output.is_symlink():
        print(f'refusing existing output: {output}', file=sys.stderr)
        return 2
    try:
        reference = load_reference(args.reference)
        native = load_json(args.native, 'native HUD JSON')
        report = compare(reference, native)
        if output.resolve() in {Path(args.reference).resolve(), Path(args.native).resolve()}:
            raise Invalid('output aliases an input path')
        output.write_text(json.dumps(report, indent=2) + '\n')
    except (Invalid, OSError, ValueError, struct.error) as exc:
        print(f'verify-battle-hud: {exc}', file=sys.stderr)
        return 2
    print(f"{report['status']}: compared {report['compared_raw_words']} raw words, "
          f"{report['compared_runtime_words']} runtime words, "
          f"{report['compared_text_bytes']} text bytes; {report['difference_count']} differences")
    return 0 if report['status'] == 'pass' else 1

if __name__ == '__main__':
    raise SystemExit(main())
