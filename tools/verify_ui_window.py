#!/usr/bin/env python3
"""Verify the bounded scalar window state from the natural battle UI constructor."""
import argparse
import json
import struct
import sys
from pathlib import Path

REFERENCE_SCHEMA = 'okage-battle-ui-v1'
NATIVE_SCHEMA = 'okage-ui-window-diagnostic-v1'
OUTPUT_SCHEMA = 'okage-ui-window-verification-v1'
ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
ENTRY_EVENT = 'ui_constructor_entry'
COMPLETE_EVENT = 'ui_constructor_complete_before_activation'
ENTRY_PC = 0x11f5f8
CONTINUATION_PC = 0x14a2c8
ROOT_GLOBAL = 0x1fce60
REGISTRY = 0x200688
STREAMS = ((0x1fd820, 29, 17, 2, 0x164090),
           (0x1fdb88, 13, 8, 3, 0x1684d0))
WINDOW_BYTES = 0x6ec0
WINDOW_WORDS = WINDOW_BYTES // 4
EXCLUDED_OFFSETS = (0x0, 0x8, 0x2698, 0x3bf0, 0x18fc, 0x650, 0x654, 0x658,
                    0x65c, 0x3d0c, 0x3d10, 0x3d14, 0x3d1c, 0x3d20, 0x53c)
EXCLUDED_WORDS = frozenset(offset // 4 for offset in EXCLUDED_OFFSETS)

class Invalid(ValueError):
    """Malformed or unsupported input; comparison would not be sound."""

def need(condition, message):
    if not condition:
        raise Invalid(message)

def u32(value, name):
    need(isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= 0xffffffff,
         f'{name}: expected unsigned 32-bit integer')
    return value

def raw(wrapper, name, size, address=None):
    need(isinstance(wrapper, dict), f'{name}: expected raw wrapper object')
    text = wrapper.get('raw_hex')
    need(isinstance(text, str), f'{name}: raw_hex is missing')
    try:
        data = bytes.fromhex(text)
    except ValueError as exc:
        raise Invalid(f'{name}: invalid raw_hex: {exc}') from exc
    need(len(text) == 2 * len(data), f'{name}: noncanonical raw_hex')
    need(wrapper.get('bytes') == len(data) == size, f'{name}: expected {size}-byte extent')
    if address is not None:
        need(wrapper.get('address') == address, f'{name}: address mismatch')
    return data

def load_json(path, label):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise Invalid(f'cannot read {label}: {exc}') from exc

def load_reference(path):
    try:
        lines = Path(path).read_text().splitlines()
        rows = [json.loads(line) for line in lines]
    except (OSError, json.JSONDecodeError) as exc:
        raise Invalid(f'cannot read reference JSONL: {exc}') from exc
    need(len(rows) == 4, f'reference requires header, two snapshots, and finish; got {len(rows)} rows')
    header, entry, complete, finish = rows
    need(all(isinstance(row, dict) for row in rows), 'reference rows must be objects')
    need(header.get('schema') == REFERENCE_SCHEMA, 'reference schema mismatch')
    need(header.get('elf_sha256') == ELF_SHA256, 'reference ELF hash mismatch')
    need(entry.get('event') == ENTRY_EVENT and complete.get('event') == COMPLETE_EVENT,
         'reference must have the ordered entry and completion snapshots')
    need((u32(entry.get('sequence'), 'entry.sequence'),
          u32(complete.get('sequence'), 'complete.sequence')) == (0, 1),
         'reference snapshot sequence is missing or duplicated')
    need(finish.get('event') == 'finish' and u32(finish.get('constructors_entered'), 'finish.constructors_entered') == 1 and
         u32(finish.get('constructors_completed'), 'finish.constructors_completed') == 1 and
         u32(finish.get('snapshots'), 'finish.snapshots') == 2,
         'reference finish row does not prove one complete constructor')
    need(u32(entry.get('frame'), 'entry.frame') == u32(complete.get('frame'), 'complete.frame'),
         'reference constructor boundaries have different frames')
    need(u32(entry.get('pc'), 'entry.pc') == ENTRY_PC and
         u32(complete.get('pc'), 'complete.pc') == CONTINUATION_PC,
         'reference boundary PCs do not match reviewed caller blocks')
    need(u32(entry.get('sp'), 'entry.sp') == u32(complete.get('sp'), 'complete.sp'),
         'reference constructor stack pointer changed across call')
    need(u32(entry.get('root'), 'entry.root') == u32(complete.get('root'), 'complete.root'),
         'reference battle root changed across call')
    need(u32(entry.get('ra'), 'entry.ra') == CONTINUATION_PC,
         'reference constructor entry return address mismatch')
    need(u32(complete.get('ra'), 'complete.ra') == CONTINUATION_PC,
         'reference caller continuation return address mismatch')
    need(entry.get('post_constructor') is False and complete.get('post_constructor') is True,
         'reference constructor boundary flags mismatch')
    u32(entry.get('fcsr'), 'entry.fcsr')
    u32(complete.get('fcsr'), 'complete.fcsr')
    root = u32(entry['root'], 'entry.root')
    for row, event_name in ((entry, 'entry'), (complete, 'complete')):
        globals_row = row.get('shared_globals')
        need(isinstance(globals_row, dict), f'reference {event_name} shared globals are missing')
        need(u32(globals_row.get('battle_root_pointer'), f'{event_name}.battle_root_pointer') == root,
             f'reference {event_name} root global does not match the paired root')
    need(u32(entry.get('constructor_sp'), 'entry.constructor_sp') == u32(entry['sp'], 'entry.sp') and
         u32(complete.get('constructor_sp'), 'complete.constructor_sp') == u32(complete['sp'], 'complete.sp'),
         'reference constructor SP metadata mismatch')
    _validate_prestate(entry)
    _validate_poststate(complete)
    return header, entry, complete, finish, root

def _validate_registry(row, expected_count, expected_slots):
    need(u32(row.get('controller_count'), 'controller_count') == expected_count,
         'reference controller count mismatch')
    registry = raw(row.get('controller_registry'), 'controller_registry', 32, REGISTRY)
    pointers = struct.unpack('<8I', registry)
    windows = row.get('controller_windows')
    need(isinstance(windows, list) and len(windows) == 8,
         'reference must include all eight controller slots')
    need(all(isinstance(w, dict) for w in windows), 'reference controller slot rows must be objects')
    need([u32(w.get('slot'), f'controller_windows[{i}].slot') for i, w in enumerate(windows)] == list(range(8)),
         'reference controller slots are incomplete or reordered')
    for slot, item in enumerate(windows):
        pointer = u32(item.get('pointer'), f'controller_windows[{slot}].pointer')
        need(pointer == pointers[slot], f'controller slot {slot} pointer differs from registry')
        if slot not in expected_slots:
            need(pointer == 0 and 'raw' not in item, f'unexpected live controller in slot {slot}')
        else:
            need(pointer != 0, f'required controller slot {slot} is null')
            raw(item.get('raw'), f'controller_windows[{slot}].raw', WINDOW_BYTES, pointer)
    return pointers

def _validate_prestate(entry):
    need(u32(entry.get('manager'), 'entry.manager') == 0, 'reference entry unexpectedly has a UI manager')
    _validate_registry(entry, 0, set())
    for key, address in [('stream_type2_written_extent', 0x1fd820),
                         ('stream_type3_written_extent', 0x1fdb88)]:
        data = raw(entry.get(key), key, 20, address)
        need(data == bytes(20), f'{key}: expected the observed empty five-word stream prestate')
    atlas = raw(entry.get('atlas_region'), 'entry.atlas_region', 16, 0x1fdba8)
    need(atlas == bytes(16), 'entry atlas first row is not the observed empty prestate')
    party = raw(entry.get('party_selection_counts_and_table'), 'entry.party_selection', 0x120, 0x1fae18)
    need(party == bytes(0x120), 'entry party-selection cache is not the observed all-zero prestate')
    need(entry.get('party_selection_cache_unchanged_since_entry') is True,
         'entry party-cache unchanged flag must be true')

def _validate_authored_table(row):
    data = raw(row.get('authored_interface_records_and_terminator'),
               'authored_interface_records_and_terminator', 0xe4, 0x248100)
    need(struct.unpack_from('<I', data, 0xe0)[0] == 0, 'authored interface terminator is nonzero')
    for index, (stream, cap_a, cap_b, layer_type, callback) in enumerate(STREAMS):
        base = index * 112
        need(struct.unpack_from('<I', data, base)[0] == stream, f'interface row {index} stream mismatch')
        need(struct.unpack_from('<I', data, base + 0x54)[0] == 0x1fdba8,
             f'interface row {index} atlas binding mismatch')
        need(struct.unpack_from('<I', data, base + 0x5c)[0] == callback and
             struct.unpack_from('<I', data, base + 0x60)[0] == layer_type,
             f'interface row {index} callback/type mismatch')
        need(struct.unpack_from('<II', data, base + 0x64) == (cap_a, cap_b),
             f'interface row {index} source capacities mismatch')

def _validate_poststate(row):
    _validate_registry(row, 2, {0, 1})
    _validate_authored_table(row)
    need(u32(row.get('manager'), 'complete.manager') != 0, 'reference manager pointer is null')
    manager = raw(row.get('manager_raw'), 'manager_raw', 0xef0)
    need(struct.unpack_from('<I', manager, 0xe40)[0] == 2 and
         struct.unpack_from('<II', manager, 0xe90) == (0, 1),
         'reference UI manager has unexpected layer count/order')
    for index, (stream, cap_a, cap_b, layer_type, callback) in enumerate(STREAMS):
        base = 0x40 + index * 0xe0
        layer = raw(row.get(f'layer{index}'), f'layer{index}', 0xe0)
        need(layer == manager[base:base + 0xe0], f'layer{index} differs from manager body')
        handle, = struct.unpack_from('<I', layer, 0)
        need(handle == index, f'layer {index} maps to the wrong controller slot')
        need(struct.unpack_from('<I', layer, 0x54)[0] == stream,
             f'layer {index} does not bind the authored stream')
        need(struct.unpack_from('<I', layer, 0x5c)[0] == callback and
             struct.unpack_from('<I', layer, 0x60)[0] == layer_type,
             f'layer {index} callback/type mismatch')
        controller = row['controller_windows'][index]
        window = raw(controller['raw'], f'controller slot {index}', WINDOW_BYTES, controller['pointer'])
        need(struct.unpack_from('<I', window, 4)[0] == index, f'window {index} slot index mismatch')
        need(struct.unpack_from('<II', window, 0x3c00) == (cap_a, cap_b),
             f'window {index} capacities do not match authored row')
    for key, address in [('stream_type2_written_extent', 0x1fd820),
                         ('stream_type3_written_extent', 0x1fdb88)]:
        raw(row.get(key), key, 20, address)
    raw(row.get('atlas_region'), 'complete.atlas_region', 16, 0x1fdba8)
    party = raw(row.get('party_selection_counts_and_table'), 'complete.party_selection', 0x120, 0x1fae18)
    need(party == bytes(0x120), 'post-constructor party-selection cache changed from observed zero prestate')
    need(row.get('party_selection_cache_unchanged_since_entry') is True,
         'post-constructor party-cache unchanged flag must be true')

def compare(reference, native):
    _header, entry, complete, _finish, _root = reference
    need(isinstance(native, dict) and set(native) == {'schema', 'scope', 'windows'} and
         native.get('schema') == NATIVE_SCHEMA,
         'native diagnostic schema mismatch')
    need(native.get('scope') == '198e38 and window configuration only; empty incoming streams and atlas, slots0/1; no layer constructors',
         'native diagnostic scope mismatch')
    windows = native.get('windows')
    need(isinstance(windows, list) and len(windows) == 2, 'native must contain exactly two windows')
    need(all(isinstance(w, dict) for w in windows), 'native window rows must be objects')
    need(all(set(w) == {'slot', 'words'} for w in windows),
         'native window rows must contain only the reviewed slot and words fields')
    need([u32(w.get('slot'), f'native slot row {i}.slot') for i, w in enumerate(windows)] == [0, 1],
         'native windows must be source-ordered slots 0 and 1')
    differences = []
    compared_words = 0
    compared_bytes = 0
    per_slot = []
    for index, item in enumerate(windows):
        values = item.get('words')
        need(isinstance(values, list) and len(values) == WINDOW_WORDS,
             f'native slot {index} requires exactly {WINDOW_WORDS} words (0x6ec0 bytes)')
        original = bytes.fromhex(complete['controller_windows'][index]['raw']['raw_hex'])
        if len(original) != WINDOW_BYTES:
            raise Invalid(f'original slot {index} controller extent mismatch')
        known = 0
        for word_index, value in enumerate(values):
            offset = word_index * 4
            if word_index in EXCLUDED_WORDS:
                need(value is None, f'native slot {index} excluded offset {offset:#x} must be null')
                continue
            actual = u32(value, f'native slot {index} word {word_index}')
            expected = struct.unpack_from('<I', original, offset)[0]
            known += 1
            if expected != actual:
                differences.append({'slot': index, 'offset': offset,
                                    'expected': expected, 'actual': actual})
        need(known == WINDOW_WORDS - len(EXCLUDED_WORDS),
             f'native slot {index} compared-word count mismatch')
        per_slot.append({'slot': index, 'window_bytes': WINDOW_BYTES,
                         'compared_words': known, 'excluded_words': len(EXCLUDED_WORDS),
                         'compared_bytes': known * 4})
        compared_words += known
        compared_bytes += known * 4
    return {
        'schema': OUTPUT_SCHEMA,
        'status': 'pass' if not differences else 'fail',
        'reference_schema': REFERENCE_SCHEMA,
        'native_schema': NATIVE_SCHEMA,
        'reference_frame': entry['frame'],
        'reference_entry_pc': entry['pc'],
        'reference_completion_pc': complete['pc'],
        'reference_root': entry['root'],
        'windows': per_slot,
        'compared_words': compared_words,
        'compared_bytes': compared_bytes,
        'excluded_offsets_hex': [hex(offset) for offset in EXCLUDED_OFFSETS],
        'first_difference': differences[0] if differences else None,
        'difference_count': len(differences),
        'differences': differences,
        'unsupported': [
            'Excluded offsets are exact pointer, allocator/accounting, or scratch fields supplied by the review; they are not compared.',
            'The native window diagnostic covers 198e38 allocation/configuration only, not layer constructors, callbacks, UI lifecycle, rendering, or full buffers.',
            'FCSR is captured by the original observer but has no native-window diagnostic equivalent.',
            'No claim is made that the observer captured host pointers or allocator accounting.'
        ]
    }

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', required=True, help='original observer JSONL')
    parser.add_argument('--native', required=True, help='native window diagnostic JSON')
    parser.add_argument('--output', required=True, help='fresh verification JSON path')
    args = parser.parse_args(argv)
    output = Path(args.output)
    if output.exists() or output.is_symlink():
        print(f'refusing existing output: {output}', file=sys.stderr)
        return 2
    try:
        reference = load_reference(args.reference)
        native = load_json(args.native, 'native JSON')
        report = compare(reference, native)
        if output.resolve() in {Path(args.reference).resolve(), Path(args.native).resolve()}:
            raise Invalid('output aliases an input path')
        output.write_text(json.dumps(report, indent=2) + '\n')
    except (Invalid, OSError, ValueError, struct.error) as exc:
        print(f'verify-ui-window: {exc}', file=sys.stderr)
        return 2
    print(f"{report['status']}: compared {report['compared_words']} words; {report['difference_count']} differences")
    return 0 if report['status'] == 'pass' else 1

if __name__ == '__main__':
    raise SystemExit(main())
