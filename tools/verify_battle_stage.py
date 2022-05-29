#!/usr/bin/env python3
"""Strict event-by-event comparison for the natural B_ROOM1 stage commit."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

REFERENCE_SCHEMA = 'okage-battle-stage-v1'
NATIVE_SCHEMA = 'okage-native-battle-stage-v1'
ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
STG_SHA256 = 'c8bd1824e9d596d440193cf56c1a0d297edbfd4bc47e970c3889f32af327b883'
STG_SIZE = 868
HEADER_SIZE = 0x278
SETTING_BEGIN, SETTING_COUNT = 0x20, 18
CAMERA_BEGIN, CAMERA_COUNT = 0x188, 8
CAMERA_SETTING_BEGIN, CAMERA_SETTING_COUNT = 0x268, 9
ENTRANCE_BEGIN, ENTRANCE_COUNT = 0x364, 0

class Invalid(ValueError):
    """Malformed input or a schema that cannot be compared safely."""

def need(condition, message):
    if not condition:
        raise Invalid(message)

def raw(wrapper, name, size=None):
    need(isinstance(wrapper, dict), f'{name}: expected raw wrapper object')
    value = wrapper.get('raw_hex')
    need(isinstance(value, str), f'{name}: missing raw_hex')
    try:
        data = bytes.fromhex(value)
    except ValueError as exc:
        raise Invalid(f'{name}: invalid hex: {exc}') from exc
    need(len(value) == len(data) * 2, f'{name}: noncanonical hex')
    need(wrapper.get('bytes') == len(data), f'{name}: byte extent mismatch')
    if size is not None:
        need(len(data) == size, f'{name}: expected {size} bytes, got {len(data)}')
    return data

def hex_bytes(value, name, size=None):
    need(isinstance(value, str), f'{name}: expected hexadecimal string')
    try:
        data = bytes.fromhex(value)
    except ValueError as exc:
        raise Invalid(f'{name}: invalid hex: {exc}') from exc
    need(len(value) == len(data) * 2, f'{name}: noncanonical hex')
    if size is not None:
        need(len(data) == size, f'{name}: expected {size} bytes, got {len(data)}')
    return data

def u32(data, offset):
    need(offset >= 0 and offset + 4 <= len(data), 'word read outside validated extent')
    return struct.unpack_from('<I', data, offset)[0]

def words(data):
    need(len(data) % 4 == 0, 'word array extent is unaligned')
    return list(struct.unpack('<' + 'I' * (len(data) // 4), data))

def stage_layout(stage):
    need(len(stage) == STG_SIZE, 'STG source extent is not the pinned 868 bytes')
    need(u32(stage, 0) == 0x00475453, 'invalid STG magic')
    need((u32(stage, 8), u32(stage, 12)) == (SETTING_BEGIN, SETTING_COUNT), 'unexpected STG settings layout')
    need((u32(stage, 16), u32(stage, 20)) == (CAMERA_BEGIN, CAMERA_COUNT), 'unexpected STG camera layout')
    need((u32(stage, 24), u32(stage, 28)) == (ENTRANCE_BEGIN, ENTRANCE_COUNT), 'unexpected STG entrance layout')
    return {
        'settings': [words(stage[SETTING_BEGIN + i * 20:SETTING_BEGIN + (i + 1) * 20]) for i in range(SETTING_COUNT)],
        'cameras': [words(stage[CAMERA_BEGIN + i * 28:CAMERA_BEGIN + (i + 1) * 28]) for i in range(CAMERA_COUNT)],
        'camera_settings': [words(stage[CAMERA_SETTING_BEGIN + i * 28:CAMERA_SETTING_BEGIN + (i + 1) * 28]) for i in range(CAMERA_SETTING_COUNT)],
        'entrances': [],
    }

def expected_event_keys():
    result = [('commit_entry', 0)]
    for index in range(SETTING_COUNT):
        result.extend([('setting_entry', index), ('setting_return', index)])
    result.extend([('settings_applied_before_free', 18),
                   ('effect_projection_sync_entry', 18),
                   ('commit_complete_before_ui', 18)])
    return result

def load_reference(path):
    try:
        lines = Path(path).read_text().splitlines()
        rows = [json.loads(line) for line in lines]
    except (OSError, json.JSONDecodeError) as exc:
        raise Invalid(f'cannot read reference JSONL: {exc}') from exc
    need(len(rows) == 42, f'reference requires header, 40 snapshots, and finish; got {len(rows)} rows')
    header = rows[0]
    finish = rows[-1]
    need(isinstance(header, dict), 'reference header must be object')
    need(isinstance(finish, dict) and finish.get('event') == 'finish', 'reference missing finish row')
    need(header.get('schema') == REFERENCE_SCHEMA, 'reference schema must be v1')
    need(header.get('elf_sha256') == ELF_SHA256, 'reference ELF hash mismatch')
    need(header.get('stg_sha256') == STG_SHA256, 'reference STG hash mismatch')
    need(finish.get('commits_completed') == 1 and finish.get('settings_completed') == SETTING_COUNT and finish.get('rows') == 40,
         'reference trace is incomplete or reports a failed commit')
    snapshots = rows[1:-1]
    need(all(isinstance(row, dict) for row in snapshots), 'reference snapshot must be object')
    need([row.get('sequence') for row in snapshots] == list(range(40)), 'reference sequence is missing, duplicate, or out of order')
    actual_keys = [(row.get('event'), row.get('setting_index')) for row in snapshots]
    need(actual_keys == expected_event_keys(), 'reference event schedule does not contain the complete natural commit')
    preimage = raw(snapshots[0].get('stg_preimage'), 'reference STG preimage', STG_SIZE)
    need(hashlib.sha256(preimage).hexdigest() == STG_SHA256, 'reference preimage hash mismatch')
    tables = stage_layout(preimage)
    for row in snapshots:
        _validate_reference_snapshot(row)
    return header, snapshots, finish, preimage, tables

def _validate_reference_snapshot(row):
    name = f"reference {row.get('event')}[{row.get('setting_index')} ]"
    for key, size in [('root_header', 0x50), ('resource_header', 0x48),
                      ('stg_table_globals', 20), ('persistent_stg_prefix', 0x278),
                      ('persistent_cameras', 0x348), ('persistent_entrances', 0x348),
                      ('persistent_camera_settings', 0x348), ('projection_globals', 0x40),
                      ('field_projection', 0x84), ('current_projection_descriptor', 0x80),
                      ('effect_projection', 16), ('light_dirty_directions_colors', 0x88),
                      ('fog', 4), ('clear', 4), ('setting_4d_signed', 4), ('filter_pointer', 4),
                      ('filter_slots', 0x120)]:
        raw(row.get(key), f'{name} {key}', size)
    suppression_word = u32(raw(row['resource_header'], f'{name} resource_header', 0x48), 0x38)
    need(suppression_word in (0, 1), f'{name}: unsupported suppression word {suppression_word}')
    if row.get('event') == 'commit_entry':
        raw(row.get('stg_preimage'), f'{name} stg_preimage', STG_SIZE)
    if row.get('event') in ('setting_entry', 'setting_return'):
        raw(row.get('setting'), f'{name} setting', 20)

def _native_u32_array(value, name, size):
    need(isinstance(value, list) and len(value) == size, f'{name}: expected {size} u32 values')
    need(all(isinstance(x, int) and not isinstance(x, bool) and 0 <= x <= 0xffffffff for x in value),
         f'{name}: elements must be unsigned 32-bit integers')
    return value

def _native_u32(value, name):
    need(isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= 0xffffffff,
         f'{name}: expected unsigned 32-bit integer')
    return value

def _native_record_array(value, name, count):
    need(isinstance(value, list) and len(value) == count, f'{name}: expected {count} records')
    return [_native_u32_array(record, f'{name}[{i}]', 7) for i, record in enumerate(value)]

def compare(reference, native):
    header, original, finish, stage, tables = reference
    need(isinstance(native, dict), 'native report must be object')
    need(native.get('schema') == NATIVE_SCHEMA, 'native report schema mismatch')
    need(isinstance(native.get('complete'), bool), 'native complete must be boolean')
    need(isinstance(native.get('error'), str), 'native error must be present as a string')
    differences = []
    unsupported = [
        'reference/native frame numbers use different clocks; no frame alignment is asserted',
        'FCSR is captured by original but absent from native report',
        'root/resource pointer identity and unclassified header fields are excluded',
        'current projection descriptor and renderer dirty descriptors have no native equivalent',
        'fog upper byte is incidental; only RGB24 is compared',
        'filter-slot opaque tails (bytes 16..31) and persistent table allocation tails are excluded',
        'filter slots 1 and 2 belong to separate capture/fade owners; their native values are not compared',
        'global filter draw enable 0x332a50 is not serialized in this report',
        'entrance allocation is empty in pinned B_ROOM1; untouched entrance allocation bytes are excluded',
    ]
    compared = {'snapshots': 0, 'settings': 0, 'projection_words': 0, 'effect_words': 0,
                'lighting_words': 0, 'stage_state_words': 0, 'filter_words': 0,
                'persistent_bytes': 0, 'source_stage_bytes': 0}
    def diff(path, expected, actual, reason='value mismatch'):
        if expected != actual:
            differences.append({'path': path, 'expected': expected, 'actual': actual, 'reason': reason})

    diff('native.complete', True, native.get('complete'))
    diff('native.error', '', native.get('error', ''))
    stage_hex = native.get('owned_stage_hex')
    if stage_hex is None:
        diff('native.owned_stage_hex', stage.hex(), '<missing>')
    else:
        native_stage = hex_bytes(stage_hex, 'native owned_stage_hex', STG_SIZE)
        diff('native.owned_stage_hex', stage.hex(), native_stage.hex())
        compared['source_stage_bytes'] += STG_SIZE
    header_hex = native.get('header_hex')
    if header_hex is None:
        diff('native.header_hex', stage[:HEADER_SIZE].hex(), '<missing>')
    else:
        native_header = hex_bytes(header_hex, 'native header_hex', HEADER_SIZE)
        diff('native.header_hex', stage[:HEADER_SIZE].hex(), native_header.hex())
        compared['persistent_bytes'] += HEADER_SIZE
    for key, actual in [('cameras', native.get('cameras')),
                        ('camera_settings', native.get('camera_settings')),
                        ('entrances', native.get('entrances'))]:
        expected = tables[key]
        if actual is None:
            diff(f'native.{key}', expected, '<missing>')
            continue
        actual_records = _native_record_array(actual, f'native.{key}', len(expected))
        diff(f'native.{key}', expected, actual_records)
        compared['persistent_bytes'] += len(expected) * 28

    snapshots = native.get('snapshots', [])
    need(isinstance(snapshots, list), 'native snapshots must be array')
    native_by_key = {}
    native_keys = []
    for index, row in enumerate(snapshots):
        need(isinstance(row, dict), f'native snapshot {index} must be object')
        event = row.get('event')
        setting_index = row.get('setting_index')
        need(isinstance(event, str) and isinstance(setting_index, int) and not isinstance(setting_index, bool),
             f'native snapshot {index} lacks event key')
        key = (event, setting_index)
        need(key not in native_by_key, f'duplicate native snapshot key {key}')
        native_by_key[key] = row
        native_keys.append(key)
    expected_keys = expected_event_keys()
    if native_keys != expected_keys:
        diff('snapshots.event_schedule', expected_keys, native_keys, 'missing, extra, or reordered native events')
    for key in expected_keys:
        orig = next(r for r in original if (r['event'], r['setting_index']) == key)
        row = native_by_key.get(key)
        if row is None:
            diff(f'snapshot[{key}].presence', 'present', 'missing', 'missing native observation')
            continue
        compared['snapshots'] += 1
        event, setting_index = key
        path = f'snapshot[{event}:{setting_index}]'
        # Validate and compare copied source tables once they have been copied by 138c98.
        if event != 'commit_entry':
            for table_name, original_name, source_bytes in [
                    ('stg_prefix', 'persistent_stg_prefix', stage[:HEADER_SIZE]),
                    ('cameras', 'persistent_cameras', stage[392:616]),
                    ('camera_settings', 'persistent_camera_settings', stage[616:868])]:
                copy_bytes = raw(orig[original_name], f'{path} {original_name}')
                extent = len(source_bytes)
                diff(f'{path}.original_copy.{table_name}', source_bytes.hex(), copy_bytes[:extent].hex(),
                     'original persistent copy differs from pinned source extent')
                compared['persistent_bytes'] += extent
        if event in ('setting_entry', 'setting_return'):
            raw_setting = raw(orig['setting'], f'{path} original setting', 20)
            expected_kind = u32(raw_setting, 0)
            expected_values = words(raw_setting[4:20])
            kind = _native_u32(row.get('kind'), f'{path}.kind')
            values = _native_u32_array(row.get('value_bits'), f'{path}.value_bits', 4)
            diff(f'{path}.kind', expected_kind, kind)
            compared['settings'] += 1
            diff(f'{path}.value_bits', expected_values, values)
            compared['settings'] += 4
        _compare_snapshot_state(path, orig, row, diff, compared)
    # The preserved filter owners 1/2 must not be changed by original commit setters.
    baseline_slots = raw(original[0]['filter_slots'], 'commit-entry filter slots', 0x120)
    for row in original[1:]:
        slots = raw(row['filter_slots'], 'filter slots', 0x120)
        for slot in (1, 2):
            base = baseline_slots[slot * 32:slot * 32 + 16]
            now = slots[slot * 32:slot * 32 + 16]
            diff(f"original_preservation[{row['event']}:{row['setting_index']}].filter_slot[{slot}]", base.hex(), now.hex(),
                 'original capture/fade slot changed during the stage commit')
            compared['filter_words'] += 4
    return {
        'schema': 'okage-battle-stage-comparison-v1',
        'reference_schema': REFERENCE_SCHEMA,
        'native_schema': native.get('schema'),
        'reference_frame_convention': header.get('frame_convention'),
        'frame_alignment': 'Reference vblank and native frame are not aligned or shifted.',
        'native_frame': native.get('frame'),
        'complete': native.get('complete') is True and not native.get('error'),
        'counts': {'reference_snapshots': len(original), 'native_snapshots': len(snapshots),
                   'matched_snapshots': compared['snapshots'], 'settings': SETTING_COUNT,
                   'differences': len(differences), 'unsupported_checks': len(unsupported)},
        'compared': compared,
        'first_difference': differences[0] if differences else None,
        'differences': differences,
        'unsupported_checks': unsupported,
    }

def _compare_snapshot_state(path, orig, native, diff, compared):
    projection = words(raw(orig['projection_globals'], f'{path} projection globals', 0x40))[:15]
    diff(f'{path}.projection', projection, _native_u32_array(native.get('projection'), f'{path}.projection', 15))
    compared['projection_words'] += 15
    effect_words = words(raw(orig['effect_projection'], f'{path} effect projection', 16))
    # Original order is far,fog-near,fog-far,near; native order is near,far,fog-near,fog-far.
    effects = [effect_words[3], effect_words[0], effect_words[1], effect_words[2]]
    diff(f'{path}.effects', effects, _native_u32_array(native.get('effects'), f'{path}.effects', 4))
    compared['effect_words'] += 4
    light = raw(orig['light_dirty_directions_colors'], f'{path} lighting state', 0x88)
    directions = words(light[0x08:0x38])
    colors = words(light[0x38:0x78])
    base = words(light[0x78:0x88])
    diff(f'{path}.directions', directions, _native_u32_array(native.get('directions'), f'{path}.directions', 12))
    diff(f'{path}.colors', colors, _native_u32_array(native.get('colors'), f'{path}.colors', 16))
    diff(f'{path}.base', base, _native_u32_array(native.get('base'), f'{path}.base', 4))
    compared['lighting_words'] += 32
    field = raw(orig['field_projection'], f'{path} field projection cache', 0x84)
    for key, offset in [('sky', 0x50), ('shadow_direction', 0x60)]:
        expected = words(field[offset:offset+16])
        diff(f'{path}.{key}', expected, _native_u32_array(native.get(key), f'{path}.{key}', 4))
        compared['stage_state_words'] += 4
    tint = words(field[0x78:0x84])
    diff(f'{path}.shadow_tint', tint, _native_u32_array(native.get('shadow_tint'), f'{path}.shadow_tint', 3))
    scalar_offsets = [('shadow_threshold',0x44),('bucket_shift',0x10),('bucket_base',0x0c),('selector',0x40)]
    for key, offset in scalar_offsets:
        diff(f'{path}.{key}', u32(field, offset), _native_u32(native.get(key), f'{path}.{key}'))
        compared['stage_state_words'] += 1
    cached = [u32(field,0x30),u32(field,0x34)]
    diff(f'{path}.cached_depth', cached, _native_u32_array(native.get('cached_depth'), f'{path}.cached_depth', 2))
    compared['stage_state_words'] += 2
    tint_count = len(tint)
    compared['stage_state_words'] += tint_count
    fog = u32(raw(orig['fog'], f'{path} fog', 4), 0) & 0x00ffffff
    diff(f'{path}.fog_rgb', fog, _native_u32(native.get('fog_rgb'), f'{path}.fog_rgb'))
    clear = u32(raw(orig['clear'], f'{path} clear', 4), 0)
    diff(f'{path}.clear_word', clear, _native_u32(native.get('clear_word'), f'{path}.clear_word'))
    signed = u32(raw(orig['setting_4d_signed'], f'{path} signed setting', 4), 0)
    diff(f'{path}.signed_setting', signed, _native_u32(native.get('signed_setting'), f'{path}.signed_setting'))
    compared['stage_state_words'] += 3
    resource = raw(orig['resource_header'], f'{path} resource header', 0x48)
    held = u32(resource,0x44) != 0
    suppression = u32(resource,0x38) != 0
    native_held = native.get('held')
    native_suppression = native.get('suppression')
    need(isinstance(native_held, bool), f'{path}.held must be boolean')
    need(isinstance(native_suppression, bool), f'{path}.suppression must be boolean')
    diff(f'{path}.held', held, native_held)
    diff(f'{path}.suppression', suppression, native_suppression)
    compared['stage_state_words'] += 2
    filter_data = raw(orig['filter_slots'], f'{path} filters', 0x120)
    native_filters = native.get('filters')
    need(isinstance(native_filters,list) and len(native_filters)==9, f'{path}.filters must contain nine slots')
    for slot in range(9):
        if slot in (1,2):
            continue
        expected = words(filter_data[slot*32:slot*32+16])
        actual = native_filters[slot]
        if actual is None:
            diff(f'{path}.filters[{slot}]', expected, None)
        else:
            values = _native_u32_array(actual, f'{path}.filters[{slot}]', 4)
            diff(f'{path}.filters[{slot}]', expected, values)
        compared['filter_words'] += 4

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', required=True)
    parser.add_argument('--native', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args(argv)
    output = Path(args.output)
    if output.exists() or output.is_symlink():
        print(f'refusing existing output: {output}', file=sys.stderr)
        return 2
    try:
        reference = load_reference(args.reference)
        try:
            native = json.loads(Path(args.native).read_text())
        except (OSError, json.JSONDecodeError) as exc:
            raise Invalid(f'cannot read native report: {exc}') from exc
        report = compare(reference, native)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report['counts'], sort_keys=True))
        return 0 if report['complete'] and not report['differences'] else 1
    except (OSError, Invalid, ValueError, TypeError, struct.error) as exc:
        print(f'battle-stage comparison input error: {exc}', file=sys.stderr)
        return 2

if __name__ == '__main__':
    raise SystemExit(main())
