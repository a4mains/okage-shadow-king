#!/usr/bin/env python3
"""Compare bounded original actor/camera callbacks with the actual native traversal.

Uses callback boundaries and source entity identities, never a fitted frame offset.
This is neither whole-scheduler equivalence nor a battle rendering comparison.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ELF = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
ACTOR_RANGES = ((0x20, 0x28), (0x2c, 0x30), (0x40, 0x50),
                (0x60, 0x70), (0x90, 0xa0), (0xc0, 0xd0),
                (0x11c, 0x120), (0x128, 0x13c), (0x17c, 0x180),
                (0x194, 0x1a0), (0x1a4, 0x1a8), (0x21c, 0x228),
                (0x310, 0x344))
NATIVE_SCOPE = 'first eligible native parent and child traversal; no resource layout, rendering or combat'
ENTRY_PC = {'parent': 0x14a248, 'camera': 0x14f170, 'party_primary': 0x153a20,
            'party_secondary': 0x153ce8, 'enemy_primary': 0x15b9a0,
            'enemy_secondary': 0x15c138, 'ui_second_layer': 0x120ad0}

def need(condition, message):
    if not condition:
        raise ValueError(message)

def binary(text, size, label):
    need(isinstance(text, str), f'{label}: missing hex')
    try:
        data = bytes.fromhex(text)
    except ValueError as error:
        raise ValueError(f'{label}: invalid hex') from error
    need(len(text) == len(data) * 2 and len(data) == size, f'{label}: extent mismatch')
    return data

def raw(row, key, size):
    obj = row.get(key)
    need(isinstance(obj, dict) and obj.get('bytes') == size, f'{key}: missing typed extent')
    return binary(obj.get('raw_hex'), size, key)

def word(data, at):
    return struct.unpack_from('<I', data, at)[0]

def half(data, at):
    return struct.unpack_from('<H', data, at)[0]

def address(value):
    return type(value) is int and 0x100000 <= value < 0x2000000 and value % 4 == 0

def reference(rows):
    need(len(rows) == 28 and all(isinstance(r, dict) for r in rows), 'expected header, 26 callback rows, finish')
    header, events, finish = rows[0], rows[1:-1], rows[-1]
    need(header.get('schema') == 'okage-battle-callbacks-v1' and header.get('source_elf_sha256') == ELF,
         'reference schema or ELF identity')
    need(finish.get('event') == 'finish' and finish.get('rows') == 26 and finish.get('ui_completed') is True,
         'reference did not finish')
    kinds = ['parent', 'camera'] + ['party_primary', 'party_secondary'] * 2 + ['enemy_primary', 'enemy_secondary'] * 3 + ['ui_second_layer']
    expected = [f'{kind}_{edge}' for kind in kinds for edge in ('entry', 'return')]
    expected[1] = 'parent_phase0_to1_return'
    need([r.get('event') for r in events] == expected, 'callback order differs from first traversal')
    need([r.get('sequence') for r in events] == list(range(26)), 'callback sequence gap/duplicate')
    root = events[0].get('root')
    need(address(root), 'invalid root identity')
    for i in range(0, 26, 2):
        entry, ret = events[i:i+2]
        need(all(address(row.get(key)) for row in (entry, ret) for key in ('owner', 'sp', 'ra', 'pc')), 'missing or invalid callback address')
        need(entry['pc'] == ENTRY_PC[kinds[i // 2]], 'incorrect callback entry PC')
        need(entry.get('post') is False and ret.get('post') is True, 'invalid callback edge')
        need(entry.get('owner') == ret.get('owner') and entry.get('sp') == ret.get('sp') and
             entry.get('ra') == ret.get('ra') == ret.get('pc'), 'unpaired callback continuation')
        need(entry.get('root') == ret.get('root') == root, 'root owner changed')
        for row in (entry, ret):
            root_data = raw(row, 'root_header', 0x81d0)
            need(row['root_header'].get('address') == root, 'root block address mismatch')
            need(word(root_data, 0x24) == (0 if row['event'] == 'parent_entry' else 1), 'root phase transition mismatch')
            if row['event'] != 'parent_entry' or row.get('camera_state') is not None:
                raw(row, 'camera_state', 0x150)
                camera = row.get('camera_owner')
                need(address(camera) and camera == word(root_data, 0x8068) == row['camera_state'].get('address'), 'camera owner mismatch')
                if row['event'].startswith('camera_'):
                    need(row['owner'] == camera, 'camera callback owner mismatch')
    actors = {}
    for i in range(4, 24, 4):
        group = events[i:i+4]
        enemy = group[0]['event'].startswith('enemy')
        owners = {r.get('owner') for r in group}
        need(len(owners) == 1, 'actor primary and secondary owners differ')
        task = raw(group[0], 'actor_owner', 0x3e0 if enemy else 0x3d0)
        key = (enemy, half(task, 0x130))
        need(key not in actors, 'duplicate source entity identity')
        for row in group:
            data = raw(row, 'actor_owner', len(task))
            need(row['actor_owner'].get('address') == row['owner'], 'actor block owner mismatch')
            need(half(data, 0x130) == key[1] and data[0x42:0x46] == task[0x42:0x46], 'actor identity changed')
        actors[key] = group
    need(sum(k[0] for k in actors) == 3 and len(actors) == 5, 'incorrect reached roster')
    return events, actors

def compare(rows, native):
    events, actors = reference(rows)
    need(isinstance(native, dict) and native.get('schema') == 'okage-native-battle-callbacks-v1' and
         native.get('scope') == NATIVE_SCOPE, 'native schema/scope mismatch')
    need(native.get('complete') is True and native.get('error') == '', 'native traversal incomplete')
    snapshots = native.get('actors')
    need(isinstance(snapshots, list) and len(snapshots) == 15, 'native snapshot count')
    by_key = {}
    for item in snapshots:
        need(isinstance(item, dict) and isinstance(item.get('enemy'), bool), 'native actor type')
        key = (item['enemy'], item.get('entity_id'), item.get('boundary'))
        need(key not in by_key and key[2] in ('before', 'after-first', 'after-second'), 'duplicate/unknown native boundary')
        task = binary(item.get('task_hex'), 0x3e0 if key[0] else 0x3d0, 'native task')
        need(half(task, 0x130) == key[1], 'native entity identity disagrees with task')
        by_key[key] = item
    need(set(by_key) == {(enemy, ident, edge) for enemy, ident in actors for edge in ('before', 'after-first', 'after-second')},
         'native roster or boundary set differs')
    need([(s['enemy'], s['entity_id'], s['boundary']) for s in snapshots] ==
         [(enemy, ident, edge) for enemy, ident in actors for edge in ('before', 'after-first', 'after-second')],
         'native actor snapshot order mismatch')
    expected_events = ['parent.activation.entry', 'parent.activation.return', 'stage.first.return',
                       'stage.second.return', 'camera.first.return', 'camera.second.return', 'party.parent.return']
    expected_events += [f'party.child.{ident}.return' for enemy, ident in actors if not enemy]
    expected_events += ['enemy.parent.return']
    expected_events += [f'enemy.child.{ident}.return' for enemy, ident in actors if enemy]
    expected_events += ['ui.first.return', 'ui.second.return']
    need(native.get('events') == expected_events, 'native callback event order mismatch')
    differences = []
    counts = {'actor_bytes': 0, 'record_bytes': 0, 'channel_words': 0, 'camera_words': 0, 'root_words': 0}

    def check(path, original, actual):
        if original != actual:
            differences.append({'path': path, 'expected': original, 'actual': actual})

    for (enemy, ident), group in actors.items():
        size = 0x3e0 if enemy else 0x3d0
        for row, boundary in zip(group, ('before', 'after-first', 'after-first', 'after-second')):
            actual = by_key[enemy, ident, boundary]
            original_task = raw(row, 'actor_owner', size)
            native_task = binary(actual['task_hex'], size, 'native task')
            path = f'actor[{ident}].{row["event"]}'
            for lo, hi in ACTOR_RANGES:
                check(f'{path}.task[{lo:03x}:{hi:03x}]', original_task[lo:hi].hex(), native_task[lo:hi].hex())
                counts['actor_bytes'] += hi - lo
            if enemy:
                check(f'{path}.primary_status[3cd]', original_task[0x3cd], native_task[0x3cd])
                counts['actor_bytes'] += 1
            record_size = 0x6c if enemy else 0x50
            original_record = raw(row, 'private_stats' if enemy else 'live_stats', record_size)
            native_record = binary(actual.get('record_hex'), record_size, 'native record')
            # 125400 resolves the first two enemy record words to string
            # pointers; the native BKDATA owner retains asset string offsets.
            # No such pointer words occur in the 0x50-byte party stat record.
            start = 8 if enemy else 0
            check(f'{path}.record[{start:x}:{record_size:x}]', original_record[start:].hex(), native_record[start:].hex())
            counts['record_bytes'] += record_size - start
            model = row.get('model')
            need(isinstance(model, dict) and isinstance(model.get('channel_count'), int), 'original model/channel count')
            count = model['channel_count']
            need(4 <= count <= 5120, 'channel count outside observer bound')
            channels = raw(model, 'channels', count * 0x90)
            values = [word(channels, channel * 0x90 + 0x20 + lane * 4) for channel in range(count) for lane in range(4)]
            target = actual.get('channels')
            need(isinstance(target, list) and all(type(v) is int and 0 <= v <= 0xffffffff for v in target), 'native channel words')
            check(f'{path}.channels.count', len(values), len(target))
            for index, (a, b) in enumerate(zip(values, target)):
                check(f'{path}.channel[{index // 4}][{index % 4}]', a, b)
                counts['channel_words'] += 1
    camera = native.get('camera')
    need(isinstance(camera, list) and len(camera) == 56 and all(type(v) is int and 0 <= v <= 0xffffffff for v in camera), 'native camera shape')
    for row in (events[3], events[-1]):
        data = raw(row, 'camera_state', 0x150)
        values = list(struct.unpack_from('<55I', data, 0x50)) + [word(data, 0x140)]
        for index, (a, b) in enumerate(zip(values, camera)):
            check(f'{row["event"]}.camera[{index}]', a, b)
            counts['camera_words'] += 1
    original_root = raw(events[-1], 'root_header', 0x81d0)
    native_root = binary(native.get('root_hex'), 0x81d0, 'native root')
    need(word(native_root, 0x24) == 1, 'native parent did not advance to phase1')
    for offset in (0x24, 0x2c, 0x50, 0x81b0, 0x81b4):
        check(f'root[{offset:x}]', word(original_root, offset), word(native_root, offset))
        counts['root_words'] += 1
    for (_, ident), group in actors.items():
        offset = 0x1d2 + ident * 0x200
        need(offset + 2 <= len(native_root), 'queued-action root index outside allocation')
        source_root = raw(group[0], 'root_header', 0x81d0)
        check(f'actor[{ident}].queued_action', half(source_root, offset), half(native_root, offset))
    counts['queued_action_halfwords'] = len(actors)
    return {'schema': 'okage-battle-callback-comparison-v1', 'status': 'fail' if differences else 'pass',
            'scope': __doc__.strip(), 'native_frame': native.get('frame'),
            'reference_frames': sorted({r.get('frame') for r in events}),
            'alignment': 'First source phase-0-to-1 traversal; callback edges and entity IDs. No temporal equality or frame shift claimed.',
            'counts': dict(counts, differences=len(differences)), 'first_difference': next(iter(differences), None),
            'differences': differences, 'actor_task_ranges': [[a, b] for a, b in ACTOR_RANGES],
            'unsupported': ['Whole scheduler, callback pointer equivalence and allocator bytes',
                            'Enemy record bytes0..7: original string pointers versus native asset offsets; string ownership/content is not observed here',
                            'Native RNG and motion contexts are absent from this callback report',
                            'Fade/projection/stage-setting originals are absent from this observer',
                            'Resource clock/layout, cache packets, graphics/audio and later combat',
                            'UI manager content uses the separate UI dispatcher verifier']}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('reference', 'native', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    result = compare([json.loads(line) for line in args.reference.read_text().splitlines()], json.loads(args.native.read_text()))
    result['inputs'] = {name: {'path': str(getattr(args, name)), 'sha256': hashlib.sha256(getattr(args, name).read_bytes()).hexdigest()}
                        for name in ('reference', 'native')}
    with args.output.open('x') as out:
        out.write(json.dumps(result, indent=2) + '\n')
    print(f'{result["status"]}: {result["counts"]["differences"]} differences; {result["counts"]["channel_words"]} channel words')
    return 0 if result['status'] == 'pass' else 1

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError, TypeError) as error:
        raise SystemExit(f'invalid: {error}') from error
