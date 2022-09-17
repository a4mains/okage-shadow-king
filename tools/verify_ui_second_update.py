#!/usr/bin/env python3
"""Compare the bounded second natural battle-UI dispatcher against native state."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

import verify_battle_hud as hud
import verify_battle_interface as interface
import verify_ui_binding as binding
import verify_ui_first_update as first
import verify_ui_window as window

Invalid, need, u32 = window.Invalid, window.need, window.u32
NATIVE_SCHEMA = 'okage-native-ui-second-update-v1'
NATIVE_SCOPE = ('isolated first and second dispatchers on copies of native Session construction state; '
                'no parent activation, resource clock, layout or rendering')
OUTPUT_SCHEMA = 'okage-ui-second-update-verification-v1'
MANAGER_NORMALIZED = {0, 4, 8, 12, 16, 20, 24, 0x44, 0x94, 0x9c,
                      0x114, 0x124, 0x174, 0x17c, 0x1f4}
SIGNED_FIELDS = ('selected_alpha_step', 'secondary_pulse_step', 'cost_pulse', 'hp_cost_gate')
HEX_EXTENTS = {'type3_formatted_hex': 256, 'type3_padded_name_hex': 256,
               'type3_current_number_hex': 64, 'type3_maximum_number_hex': 64}
SNAPSHOT_FIELDS = {
    'manager_words', 'windows', 'hud', 'party_counts', 'party_rows',
    'resource_accumulators', 'command_changed', 'command_values', 'command',
    'selected_alpha_step', 'secondary_pulse', 'secondary_pulse_step',
    'cost_pulse', 'hp_cost_gate', 'type3_blink', *HEX_EXTENTS,
    'selected_alpha', 'formation_count', 'formation_unit_ids',
    'formation_counts', 'formation_groups',
}

def parse(text):
    return interface.parse(text)

def load_reference(path):
    return binding.load_reference(path)

def _word(data, offset):
    return struct.unpack_from('<I', data, offset)[0]

def _signed(value, label):
    need(type(value) is int and -(1 << 31) <= value < (1 << 31),
         f'{label}: expected signed32')
    return value

def _hex(value, size, label):
    need(isinstance(value, str), f'{label}: expected hex string')
    try:
        result = bytes.fromhex(value)
    except ValueError as exc:
        raise Invalid(f'{label}: malformed hex') from exc
    need(len(value) == size * 2 and len(result) == size, f'{label}: expected {size} bytes')
    return result

def _original_manager(reference_row):
    manager = u32(reference_row.get('manager'), 'reference manager')
    root = u32(reference_row.get('root'), 'reference root')
    need(manager and root and manager != root, 'invalid paired manager/root identity')
    raw = binding.raw(reference_row.get('manager_raw'), 'second-update original manager',
                      interface.MANAGER_BYTES, manager)
    need(tuple(_word(raw, off) for off in (0, 4, 8)) == interface.CALLBACKS,
         'original manager callback table mismatch')
    need(_word(raw, 0x14) == root, 'original manager parent/root relation mismatch')
    need(_word(raw, 0x10) == 0 and _word(raw, 0x18) == 0,
         'original manager next-sibling/child links must be null')
    root_bytes = interface.raw(reference_row, 'battle_root', 0x81d0, root)
    children = tuple(_word(root_bytes, at) for at in (0x8064, 0x8068, 0x806c, 0x8070))
    need(all(children) and len(set(children)) == len(children),
         'original battle-root child owners are null or overlap')
    need(root not in children and manager not in children,
         'original manager/root overlaps a battle-root child owner')
    need(_word(root_bytes, 0x8060) == manager,
         'original root manager owner does not match captured manager')
    enemy_owner = children[3]
    need(enemy_owner and _word(raw, 0xc) == enemy_owner,
         'original manager previous-sibling link does not match root enemy owner')
    need(_word(raw, 0x114) == manager and _word(raw, 0x1f4) == manager,
         'second callback must begin with both layer backlinks established')
    need(_word(raw, 0xe40) == 2 and _word(raw, 0xe90) == 0 and _word(raw, 0xe94) == 1,
         'original active layer count/order mismatch')
    for i, (stream, _a, _b, typ, callback) in enumerate(window.STREAMS):
        base = 0x40 + i * 0xe0
        need(_word(raw, base + 0x54) == stream and _word(raw, base + 0x5c) == callback and
             _word(raw, base + 0x60) == typ,
             f'original layer {i} stream/callback/type identity mismatch')
        need(_word(raw, base + 0xd4) == manager,
             f'original layer {i} parent manager backlink mismatch')
    return manager, root, raw

def _compare_manager(reference_row, native_words, label):
    manager, root, original = _original_manager(reference_row)
    need(isinstance(native_words, list) and len(native_words) == interface.MANAGER_WORDS,
         f'{label}: expected {interface.MANAGER_WORDS} manager words')
    actual = [u32(value, f'{label}.manager_words[{i}]') for i, value in enumerate(native_words)]
    normalize = {
        0: (0, 'first dispatcher function'), 4: (0, 'second dispatcher function'),
        8: (0, 'manager destructor'), 0xc: (0, 'previous sibling owner'),
        0x10: (0, 'next sibling'), 0x14: (0, 'parent root owner'),
        0x18: (0, 'child link'), 0x44: (1, 'shared texture binding'),
        0x94: (1, 'type2 stream binding'), 0x9c: (1, 'type2 callback binding'),
        0x114: (1, 'type2 manager backlink'), 0x124: (1, 'shared texture binding'),
        0x174: (2, 'type3 stream binding'), 0x17c: (2, 'type3 callback binding'),
        0x1f4: (1, 'type3 manager backlink'),
    }
    differences, compared = [], 0
    for offset in range(0, interface.MANAGER_BYTES, 4):
        expected = _word(original, offset)
        got = actual[offset // 4]
        if offset in normalize:
            native_expected, role = normalize[offset]
            if offset in (0, 4, 8):
                need(expected == interface.CALLBACKS[offset // 4], f'original {role} changed')
            elif offset == 0xc:
                root_bytes = interface.raw(reference_row, 'battle_root', 0x81d0, root)
                need(expected == _word(root_bytes, 0x8070), 'original previous-sibling owner mismatch')
            elif offset == 0x14:
                need(expected == root, 'original manager parent does not match root')
            elif offset in (0x10, 0x18):
                need(expected == 0, f'original {role} must be null')
            elif offset in (0x44, 0x124):
                need(expected == u32(reference_row['shared_globals'].get('battle_ui_texture'), 'source texture'),
                     'original manager texture pointer mismatch')
            elif offset in (0x94, 0x174):
                need(expected == window.STREAMS[(offset - 0x94) // 0xe0][0],
                     'original manager stream pointer mismatch')
            elif offset in (0x9c, 0x17c):
                need(expected == window.STREAMS[(offset - 0x9c) // 0xe0][4],
                     'original manager callback pointer mismatch')
            elif offset in (0x114, 0x1f4):
                need(expected == manager, 'original manager backlink mismatch')
            if got != native_expected:
                differences.append({'component': 'manager', 'phase': label, 'offset': offset,
                                    'role': role, 'expected': native_expected, 'actual': got})
        else:
            compared += 1
            if expected != got:
                differences.append({'component': 'manager', 'phase': label, 'offset': offset,
                                    'expected': expected, 'actual': got})
    need(MANAGER_NORMALIZED == set(normalize), 'manager normalization mask mismatch')
    return differences, compared

def _expected_commands(reference_row, slot):
    embeds = reference_row.get('embedded_streams')
    need(isinstance(embeds, list) and len(embeds) == 2, 'original embedded stream rows missing')
    embed = embeds[slot]
    pointer = u32(reference_row['controller_windows'][slot]['pointer'], 'window pointer')
    need(embed.get('slot') == slot and embed.get('window') == pointer and
         embed.get('active_stream_pointer') == pointer + 0x26dc and
         embed.get('embedded_stream_active') is True,
         f'original window {slot} embedded stream owner mismatch')
    cursor = u32(embed.get('cursor'), 'embedded cursor')
    need(embed.get('word_count_including_terminator') == cursor + 1,
         'original embedded stream count mismatch')
    words = embed.get('words')
    need(isinstance(words, list) and len(words) == cursor + 1,
         'original embedded stream word extent mismatch')
    expected = [{'opcode': 14, 'binding': 'ruw_metrics'},
                {'opcode': 6, 'binding': 'ruw_clip'}]
    source_window = binding.raw(reference_row['controller_windows'][slot].get('raw'),
                                f'original window {slot}', window.WINDOW_BYTES, pointer)
    need(_word(source_window, 0x2698) == pointer + 0x26dc,
         f'original window {slot} active stream pointer mismatch')
    raw_owner = u32(reference_row.get('ruw_slot0', {}).get('raw_pointer'), 'RUW raw owner')
    group_set = (2, 3, 4) if slot == 0 else (5,)
    idx = 0
    for group in group_set:
        for index in range(hud.GROUP_COUNTS[group]):
            word_index = 4 + 2 * idx
            need(words[word_index] == 23, 'original widget stream opcode mismatch')
            offset = hud.GROUPS_OFFSET + (hud.GROUP_RECORD_OFFSETS[group] + index) * hud.RECORD_BYTES
            arg = words[word_index + 1]
            need(isinstance(arg, dict) and arg == {'raw_widget_offset': offset},
                 'original widget pointer offset/order mismatch')
            need(_word(source_window, 0x26dc + (word_index + 1) * 4) == raw_owner + offset,
                 'original embedded widget pointer does not resolve into RUW raw owner')
            expected.append({'opcode': 23, 'group': group, 'index': index})
            idx += 1
    need(words[0:4] == [14, 0x200b78, 6, 0x200b88] and words[cursor] == 0,
         'original embedded command prefix/terminator mismatch')
    need(_word(source_window, 0x26dc) == 14 and _word(source_window, 0x26e4) == 6 and
         _word(source_window, 0x26dc + 4) == 0x200b78 and
         _word(source_window, 0x26dc + 12) == 0x200b88,
         'original embedded stream prefix does not match authored defaults')
    need(cursor == 4 + 2 * idx, 'original embedded widget cursor mismatch')
    capacity = _word(source_window, 0x3c00)
    extent = (capacity + 1) * 256
    need(u32(embed.get('glyph_buffer_capacity'), 'glyph capacity') == capacity and
         u32(embed.get('glyph_buffer_allocation_count'), 'glyph allocation count') == capacity + 1 and
         u32(embed.get('glyph_buffer_extent_bytes'), 'glyph extent') == extent,
         'source glyph buffer bounds do not match window capacity')
    for name, offset in (('glyph_positions', 0x650), ('glyph_metrics', 0x654)):
        pointer = u32(embed.get(name + '_pointer'), name + '_pointer')
        need(pointer and pointer == _word(source_window, offset),
             f'original {name} pointer does not match window owner')
        binding.raw(embed.get(name), f'original {name}', extent, pointer)
    return expected, embed

def _compare_windows(reference_row, native_windows, phase):
    need(isinstance(native_windows, list) and len(native_windows) == 2,
         f'{phase}: exactly two native windows required')
    source_diffs, compared, per_slot = first._compare_windows(
        reference_row, native_windows, f'{phase}.window', after=True)
    glyph_bytes = 0
    for slot, native in enumerate(native_windows):
        need(isinstance(native, dict) and set(native) == {
            'slot', 'scalar_words', 'commands', 'glyph_positions_hex', 'glyph_metrics_hex'},
            f'{phase} window {slot} schema mismatch')
        need(u32(native.get('slot'), f'{phase} slot') == slot, 'native window order mismatch')
        commands, embed = _expected_commands(reference_row, slot)
        actual_commands = native.get('commands')
        need(isinstance(actual_commands, list) and len(actual_commands) == len(commands),
             f'{phase} window {slot} command extent mismatch')
        for i, (want, got) in enumerate(zip(commands, actual_commands)):
            need(isinstance(got, dict) and got == want,
                 f'{phase} window {slot} command {i} binding/order mismatch')
        extent = u32(embed.get('glyph_buffer_extent_bytes'), 'glyph extent')
        need(extent > 0 and extent <= 33 * 256 and extent % 256 == 0,
             'source glyph buffer extent outside reviewed bound')
        for key in ('glyph_positions', 'glyph_metrics'):
            wrapper = embed.get(key)
            expected = binding.raw(wrapper, f'original {phase} {key}', extent,
                                   u32(embed.get(key + '_pointer'), key + '_pointer'))
            actual = _hex(native[key + '_hex'], extent, f'{phase}.{key}')
            if expected != actual:
                diff_at = next(i for i, pair in enumerate(zip(expected, actual)) if pair[0] != pair[1])
                source_diffs.append({'component': 'glyph', 'phase': phase, 'slot': slot,
                                     'field': key, 'different_bytes': sum(a != b for a, b in zip(expected, actual)),
                                     'first_offset': diff_at, 'expected': expected[diff_at],
                                     'actual': actual[diff_at]})
            glyph_bytes += extent
        per_slot[slot] = {'slot': slot, 'compared_scalar_words': per_slot[slot],
                          'compared_commands': len(commands),
                          'compared_glyph_bytes': 2 * extent}
    return source_diffs, compared, per_slot, glyph_bytes

def _compare_hud(reference_tuple, reference_row, native_hud, phase):
    _header, rows, finish = reference_tuple
    other = next((r for r in rows if r is not reference_row), rows[0])
    projected = (_header, other, reference_row, finish, u32(reference_row.get('root'), 'root'))
    need(isinstance(native_hud, dict) and native_hud.get('scope') ==
         'RUW and HUD state at an isolated dispatcher boundary' and
         native_hud.get('execution') == 'ui-update',
         f'{phase} HUD diagnostic boundary identity mismatch')
    adapted = dict(native_hud)
    adapted['scope'] = hud.NATIVE_SCOPE
    adapted['execution'] = 'isolated'
    report = hud.compare(projected, adapted)
    return report

def _compare_snapshot(reference_tuple, reference_row, snapshot, phase):
    need(isinstance(snapshot, dict) and set(snapshot) == SNAPSHOT_FIELDS,
         f'{phase} snapshot fields mismatch')
    manager_diffs, manager_words = _compare_manager(reference_row, snapshot['manager_words'], phase)
    window_diffs, window_words, window_slots, glyph_bytes = _compare_windows(
        reference_row, snapshot['windows'], phase)
    hud_report = _compare_hud(reference_tuple, reference_row, snapshot['hud'], phase)

    cache = binding.raw(reference_row.get('party_selection_counts_and_table'),
                        f'{phase} original party cache', 0x120, 0x1fae18)
    counts = struct.unpack_from('<8I', cache)
    rows = struct.unpack_from('<64i', cache, 32)
    native_counts = snapshot.get('party_counts')
    native_rows = snapshot.get('party_rows')
    need(isinstance(native_counts, list) and len(native_counts) == 8,
         f'{phase} party_counts extent mismatch')
    need(isinstance(native_rows, list) and len(native_rows) == 8 and
         all(isinstance(row, list) and len(row) == 8 for row in native_rows),
         f'{phase} party_rows extent mismatch')
    actual_counts = [u32(v, f'{phase}.party_counts[{i}]') for i, v in enumerate(native_counts)]
    actual_rows = [_signed(v, f'{phase}.party_rows[{r}][{c}]')
                   for r, row in enumerate(native_rows) for c, v in enumerate(row)]
    differences = manager_diffs + window_diffs
    differences.extend({'component': 'hud', 'phase': phase, **diff}
                       for diff in hud_report['differences'])
    for i, (expected, actual) in enumerate(zip(counts, actual_counts)):
        if expected != actual:
            differences.append({'component': 'party_cache', 'phase': phase,
                                'field': f'counts[{i}]', 'expected': expected, 'actual': actual})
    for i, (expected, actual) in enumerate(zip(rows, actual_rows)):
        if expected != actual:
            differences.append({'component': 'party_cache', 'phase': phase,
                                'field': f'rows[{i // 8}][{i % 8}]', 'expected': expected, 'actual': actual})

    source_acc = binding.raw(reference_row.get('type2_widget_index_accumulators'),
                             f'{phase} source accumulators', 64, 0x200a88)
    expected_acc = struct.unpack('<16I', source_acc)
    native_acc = snapshot.get('resource_accumulators')
    need(isinstance(native_acc, list) and len(native_acc) == 16,
         f'{phase} resource accumulator extent mismatch')
    actual_acc = [u32(v, f'{phase}.resource_accumulators[{i}]') for i, v in enumerate(native_acc)]
    for i, (expected, actual) in enumerate(zip(expected_acc, actual_acc)):
        if expected != actual:
            differences.append({'component': 'resource_accumulators', 'phase': phase,
                                'word': i, 'expected': expected, 'actual': actual})

    changed = snapshot.get('command_changed')
    values = snapshot.get('command_values')
    need(isinstance(changed, list) and len(changed) == 8, f'{phase} command_changed extent mismatch')
    need(isinstance(values, list) and len(values) == 8, f'{phase} command_values extent mismatch')
    for i, value in enumerate(changed): u32(value, f'{phase}.command_changed[{i}]')
    for i, value in enumerate(values): u32(value, f'{phase}.command_values[{i}]')
    for key in ('command', 'secondary_pulse', 'type3_blink'):
        u32(snapshot.get(key), f'{phase}.{key}')
    for key in SIGNED_FIELDS: _signed(snapshot.get(key), f'{phase}.{key}')
    scratch_bytes = {}
    for key, size in HEX_EXTENTS.items():
        scratch_bytes[key] = _hex(snapshot.get(key), size, f'{phase}.{key}')
    # The current selection alpha itself is source-captured in the 1fd770 block.
    state = binding.raw(reference_row.get('battle_ui_party_refresh_state'),
                        f'{phase} source selected state', 32, 0x1fd770)
    selected_alpha = _word(state, 8)
    actual_alpha = u32(snapshot.get('selected_alpha'), f'{phase}.selected_alpha')
    if selected_alpha != actual_alpha:
        differences.append({'component': 'shared_global', 'phase': phase,
                            'field': 'selected_alpha', 'expected': selected_alpha,
                            'actual': actual_alpha})

    u32(snapshot.get('formation_count'), f'{phase}.formation_count')
    for key in ('formation_unit_ids', 'formation_counts', 'formation_groups'):
        data = snapshot.get(key)
        need(isinstance(data, list) and len(data) == 8, f'{phase}.{key} extent mismatch')
        for i, value in enumerate(data): _signed(value, f'{phase}.{key}[{i}]')

    return {
        'differences': differences,
        'manager_words_compared': manager_words,
        'window_words_compared': window_words,
        'party_words_compared': 72,
        'accumulator_words_compared': 16,
        'glyph_bytes_compared': glyph_bytes,
        'hud': hud_report,
        'windows': window_slots,
        'source_selected_alpha': selected_alpha,
        'native_scratch_bytes_validated_uncompared': {k: len(v) for k, v in scratch_bytes.items()},
    }

def compare(reference, native):
    header, rows, finish = reference
    entry, complete = rows[2], rows[3]
    all_windows = [binding.windows(row) for row in rows]
    need(all([pointer for pointer, _data in all_windows[0]] ==
             [pointer for pointer, _data in owners] for owners in all_windows[1:]),
         'reference controller window owners changed during dispatcher trace')
    before_ruw, after_ruw = entry.get('ruw_slot0'), complete.get('ruw_slot0')
    need(isinstance(before_ruw, dict) and isinstance(after_ruw, dict) and
         all(before_ruw.get(key) == after_ruw.get(key)
             for key in ('raw_pointer', 'runtime_pointer', 'text_pointer')),
         'reference RUW owners changed during second callback')
    allowed = {'schema', 'scope', 'snapshots', 'frame', 'base_report', 'input_hashes'}
    need(isinstance(native, dict) and set(native) <= allowed and
         {'schema', 'scope', 'snapshots'} <= set(native), 'native second-update report fields mismatch')
    need(native.get('schema') == NATIVE_SCHEMA and native.get('scope') == NATIVE_SCOPE,
         'native second-update schema/scope mismatch')
    if 'frame' in native: u32(native['frame'], 'native.frame')
    if 'base_report' in native: need(isinstance(native['base_report'], dict), 'base_report must be an object')
    if 'input_hashes' in native:
        need(isinstance(native['input_hashes'], dict) and all(isinstance(k, str) and isinstance(v, str)
             and len(v) == 64 and all(ch in '0123456789abcdef' for ch in v)
             for k, v in native['input_hashes'].items()), 'input_hashes must map names to lowercase SHA256')
    snapshots = native.get('snapshots')
    need(isinstance(snapshots, dict) and set(snapshots) == {'before', 'after'},
         'native must contain exactly the before/after second-update states')
    before = _compare_snapshot(reference, entry, snapshots['before'], 'before')
    after = _compare_snapshot(reference, complete, snapshots['after'], 'after')

    differences = before['differences'] + after['differences']
    return {
        'schema': OUTPUT_SCHEMA,
        'status': 'fail' if differences else 'pass',
        'scope': NATIVE_SCOPE,
        'reference_schema': header.get('schema'),
        'reference_second_callback_frames': [entry['frame'], complete['frame']],
        'native_frame': native.get('frame'),
        'snapshots': {
            'before': {k: v for k, v in before.items() if k != 'differences'},
            'after': {k: v for k, v in after.items() if k != 'differences'},
        },
        'compared_manager_words': before['manager_words_compared'] + after['manager_words_compared'],
        'compared_window_words': before['window_words_compared'] + after['window_words_compared'],
        'compared_party_cache_words': 144,
        'compared_accumulator_words': 32,
        'compared_glyph_bytes': before['glyph_bytes_compared'] + after['glyph_bytes_compared'],
        'compared_hud': {'before': before['hud'], 'after': after['hud']},
        'unsupported': [
            'The original trace has no variable-owner snapshot, so native variables are not compared.',
            'Original type3 formatted/padded scratch arrays, command-state scratch, and formation summary arrays are absent; native values are extent/type-validated only. type3_blink is also not compared because its original source owner was not captured.',
            'The original trace does not capture the selected-alpha pulse step, secondary pulse, cost pulse or HP gate owners; those native scalars are validated but not compared.',
            'The comparison covers the first 120ad0 second-pass invocation (164090 then 1684d0) on copied native state; it does not establish parent activation, resource-clock scheduling, layout, rendering, or later UI progression.',
            f'HUD shared-global comparison is limited to these ten keys: {", ".join(hud.GLOBAL_KEYS)}. HUD text compares only bytes through the first NUL terminator; text tails are excluded.',
            'The native report frame is recorded independently; this tool does not align it temporally with the emulator frame.'
        ],
        'difference_count': len(differences),
        'first_difference': differences[0] if differences else None,
        'differences': differences,
    }

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference', required=True)
    p.add_argument('--native', required=True)
    p.add_argument('--output', required=True)
    args = p.parse_args(argv)
    output = Path(args.output)
    if output.exists() or output.is_symlink():
        print(f'refusing existing output: {output}', file=sys.stderr)
        return 2
    try:
        reference = load_reference(args.reference)
        native_path = Path(args.native)
        native = parse(native_path.read_text())
        if output.resolve() in {Path(args.reference).resolve(), native_path.resolve()}:
            raise Invalid('output aliases an input path')
        report = compare(reference, native)
        report['inputs'] = {name: {'path': str(Path(path).resolve()),
                                   'sha256': hashlib.sha256(Path(path).read_bytes()).hexdigest()}
                            for name, path in (('original', args.reference), ('native', args.native))}
        modules = (hud, interface, binding, first, window)
        report['verifier_sha256'] = {Path(m.__file__).name: hashlib.sha256(Path(m.__file__).read_bytes()).hexdigest()
                                     for m in modules}
        report['verifier_sha256'][Path(__file__).name] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        with output.open('x', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2)
            stream.write('\n')
        print(f"{report['status']}: {report['difference_count']} differences; "
              f"{report['compared_glyph_bytes']} glyph bytes")
        return 0 if report['status'] == 'pass' else 1
    except (Invalid, OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        print(f'UI second-update verification error: {exc}', file=sys.stderr)
        return 2

if __name__ == '__main__':
    raise SystemExit(main())
