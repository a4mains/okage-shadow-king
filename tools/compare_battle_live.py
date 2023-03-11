#!/usr/bin/env python3
"""Fixed-anchor live callback differential; no alignment search or pointer fitting.

Source scalar ranges follow compare_battle_construction and battle_state.cpp.
Only the current roster's phase/mask is observed in source owner_header. Other
roster state on parent rows is deliberately not inferred or carried forward.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import struct
import sys
from compare_battle_construction import COMMON, EXTRA

ELF = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
PCS = {'parent': {0x14a248, 0x14a508, 0x1519f8, 0x151e68},
       'party_roster': {0x1524e0}, 'enemy_roster': {0x15afa0}}
ROOT = [(0x2c, 0x30), (0x3c, 0x48), (0x50, 0x54)]
TAIL = [(0x807c, 0x8084), (0x80a0, 0x81c4)]
ENTITY = [(0, 0x16e), (0x170, 0x1f4), (0x1fc, 0x200)]

class Invalid(ValueError):
    pass

def need(condition, message):
    if not condition:
        raise Invalid(message)

def unhex(value, size, label):
    need(isinstance(value, str), label + ': missing hex')
    try:
        data = bytes.fromhex(value)
    except ValueError as error:
        raise Invalid(label + ': invalid hex') from error
    need(len(value) == len(data)*2 and len(data) == size, label + ': wrong extent/noncanonical hex')
    return data

def raw(row, key, size):
    obj = row.get(key)
    need(isinstance(obj, dict) and obj.get('bytes') == size, key + ': wrong recorded extent')
    return unhex(obj.get('raw_hex'), size, key)

def word(data, offset):
    return struct.unpack_from('<I', data, offset)[0]

def load(path, original):
    try:
        rows = [json.loads(line) for line in Path(path).read_text().splitlines()]
    except (ValueError, OSError) as error:
        raise Invalid('Unreadable JSONL: ' + str(error)) from error
    schema = 'okage-battle-live-v1' if original else 'okage-native-battle-live-v1'
    need(rows and isinstance(rows[0], dict) and rows[0].get('schema') == schema, 'Wrong trace schema')
    header = rows.pop(0)
    if original:
        need(header.get('source_elf_sha256') == ELF, 'Wrong source ELF')
        need(type(header.get('requested_vblanks')) is int and 0 < header['requested_vblanks'] <= 4096, 'Invalid original interval')
    finish = rows.pop() if rows and rows[-1].get('event') == 'finish' else None
    pending = None
    previous = -1
    for index, row in enumerate(rows):
        need(row.get('event') == 'callback' and row.get('sequence') == index, 'Missing/duplicate/out-of-order sequence')
        frame = row.get('frame')
        need(type(frame) is int and frame >= previous, 'Invalid/backward frame')
        if original:
            need(frame < header['requested_vblanks'], 'Source frame exceeds interval')
        previous = frame
        need(row.get('entry_pc') in PCS.get(row.get('kind'), set()), 'Unknown callback kind/PC')
        identity = (row.get('call_id'), row['kind'], row['entry_pc'])
        if row.get('edge') == 'entry':
            need(pending is None, 'Nested/unpaired entry')
            pending = identity
        else:
            need(row.get('edge') == 'return' and pending == identity, 'Unpaired/wrong return')
            pending = None
    if finish:
        need(pending is None, 'Finish with unpaired entry')
        if original:
            need(finish.get('rows') == len(rows) and finish.get('pairs')*2 == len(rows), 'Finish row counts')
            need(finish.get('completed_vblanks') == header['requested_vblanks'], 'Incomplete original finish')
    need(not original or finish is not None, 'Original missing finish')
    return header, rows, {'finish_present': finish is not None, 'unpaired_entry': pending}

def normalize(row, original):
    result = {}
    prefix = raw(row, 'root_prefix', 0x60) if original else unhex(row.get('root_prefix_hex'), 0x60, 'root prefix')
    tail = raw(row, 'root_tail_queue', 0x170) if original else unhex(row.get('root_tail_queue_hex'), 0x170, 'root tail')
    result['parent_callback'] = word(prefix, 0) if original else row['parent_callback']
    result['parent_phase'] = word(prefix, 0x24) if original else row['parent_phase']
    if row['kind'] != 'parent':
        kind = 'party' if row['kind'] == 'party_roster' else 'enemy'
        owner = raw(row, 'owner_header', 0x38) if original else None
        result[kind+'_phase'] = word(owner, 0x24) if original else row[kind+'_phase']
        result[kind+'_mask'] = (word(owner, 0x1c) if original else row[kind+'_mask']) & 0x18000000
    for lo, hi in ROOT:
        result[f'root[{lo:x}:{hi:x}]'] = prefix[lo:hi].hex()
    for lo, hi in TAIL:
        result[f'root[{lo:x}:{hi:x}]'] = tail[lo-0x8060:hi-0x8060].hex()
    count = word(prefix, 0x50)
    need(count <= 64, 'Entity count exceeds64')
    if original:
        need(row.get('entity_count') == count, 'Entity count disagrees')
    entities = raw(row, 'registered_entities', count*0x200) if original else unhex(row.get('registered_entities_hex'), count*0x200, 'entities')
    for i in range(count):
        for lo, hi in ENTITY:
            result[f'entity[{i}][{lo:x}:{hi:x}]'] = entities[i*0x200+lo:i*0x200+hi].hex()
    result['entry'] = (raw(row, 'battle_entry', 0x38) if original else unhex(row.get('entry_hex'), 0x38, 'entry')).hex()
    result['shared_variables_574'] = word(raw(row, 'shared_variables_574', 4), 0) if original else row['shared_variables_574']
    result['rng_state_word'] = row['rng_state_word']
    actors = row.get('actors')
    need(isinstance(actors, list), 'Missing actors')
    ids = [a['entity_id'] for a in actors]
    need(len(ids) == len(set(ids)) and all(type(i) is int and 0 <= i < count for i in ids), 'Invalid/duplicate actor identity')
    result['actor_ids'] = sorted(ids)
    for actor in actors:
        name = f"actor[{actor['entity_id']}]"
        enemy = actor['type'] == 1 if original else actor['enemy']
        result[name+'.enemy'] = enemy
        task_size, record_size = (0x3e0, 0x6c) if enemy else (0x3d0, 0x50)
        task = raw(actor, 'task', task_size) if original else unhex(actor.get('task_hex'), task_size, name)
        for lo, hi in COMMON + EXTRA['enemy' if enemy else 'party']:
            result[f'{name}.task[{lo:x}:{hi:x}]'] = task[lo:hi].hex()
        result[name+'.record'] = (raw(actor, 'record', record_size) if original else unhex(actor.get('record_hex'), record_size, name+'.record')).hex()
        bindings = actor.get('stat_bindings')
        need(isinstance(bindings, list) and len(bindings) == 6, 'Binding count')
        for j, size in enumerate([4, 1, 2, 1, 2, 2]):
            binding = bindings[j]
            if original:
                if enemy and j == 5:
                    need(binding.get('present') is False and binding.get('raw_pointer') == 0 and 'value' not in binding, 'Enemy sixth source binding must be absent')
                    value = None
                else:
                    value = raw(binding, 'value', size).hex()
            else:
                value = None if binding is None else unhex(binding, size, name+'.binding').hex()
            result[f'{name}.binding[{j}]'] = value
        if original:
            model = actor['model']; n = model.get('captured_channels')
            need(type(n) is int and 3 <= n <= 4, 'Source captured channel count')
            channels = raw(model, 'world_channels_raw', n*0x90)
            values = [list(struct.unpack_from('<4I', channels, i*0x90+0x20)) for i in range(n)]
        else:
            values = actor.get('world_channel_bits')
            need(isinstance(values, list) and 3 <= len(values) <= 4 and all(len(v) == 4 for v in values), 'Native channel shape')
        result[name+'.channel_count'] = len(values)
        for j, value in enumerate(values):
            for lane, bits in enumerate(value):
                result[f'{name}.channel[{j}][{lane}]'] = bits
    return result

def compare(original, native, anchor):
    need(type(anchor) is int and anchor >= 0, 'Explicit nonnegative source anchor required')
    oh, source, oc = load(original, True); _, target, nc = load(native, False)
    end = anchor + oh['requested_vblanks'] - 1
    def indexed(rows, offset):
        counts = Counter(); out = {}; orders = defaultdict(list)
        for row in rows:
            frame = row['frame'] + offset
            if not anchor <= frame <= end:
                continue
            base = (frame, row['kind'], row['entry_pc'], row['edge'])
            key = base+(counts[base],); counts[base] += 1
            out[key] = row; orders[frame].append(list(key[1:]))
        return out, orders
    a, ao = indexed(source, anchor); b, bo = indexed(target, 0)
    missing_native = sorted(a.keys()-b.keys()); missing_original = sorted(b.keys()-a.keys())
    differences = []; total = 0; counts = Counter(); matched_rows = 0
    phase_changes = {'original': [], 'native': []}
    for side, rows, shift in [('original', source, anchor), ('native', target, 0)]:
        last = None
        for row in rows:
            if row['kind'] != 'parent' or row['edge'] != 'entry': continue
            value = [row['entry_pc'], word(raw(row, 'root_prefix', 0x60), 0x24) if side == 'original' else row['parent_phase']]
            if value != last: phase_changes[side].append({'frame':row['frame']+shift,'entry_pc':value[0],'phase':value[1]}); last=value
    for key in sorted(a.keys() & b.keys(), key=lambda key:(key[0], a[key]['sequence'])):
        av, bv = normalize(a[key], True), normalize(b[key], False)
        row_equal = True
        for field in sorted(av.keys() | bv.keys()):
            if av.get(field) != bv.get(field):
                row_equal = False; total += 1; counts[field] += 1
                if len(differences) < 200:
                    differences.append({'key':key,'field':field,'original':av.get(field),'native':bv.get(field)})
        matched_rows += row_equal
    order_differences = [{'frame':f,'original':ao[f],'native':bo[f]} for f in sorted(ao.keys()|bo.keys()) if ao[f] != bo[f]]
    first_observation = min([{'frame':k[0],'issue':'missing_native','key':k} for k in missing_native] +
                            [{'frame':k[0],'issue':'missing_original','key':k} for k in missing_original],
                            key=lambda item:item['frame'], default=None)
    return {'schema':'okage-battle-live-comparison-v1','anchor':anchor,'end':end,
            'inputs':{str(p):hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in [original,native]},
            'coverage':{'original':oc,'native':nc,'original_rows_in_interval':len(a),'native_rows_in_interval':len(b),
                        'native_rows_outside_interval':len(target)-len(b),'paired_rows':len(a.keys()&b.keys()),'equal_rows':matched_rows},
            'missing_native':missing_native,'missing_original':missing_original,'order_differences':order_differences,
            'first_observation_mismatch':first_observation,'phase_changes':phase_changes,'field_mismatch_count':total,'field_counts':dict(counts),
            'first_mismatch':differences[0] if differences else None,'mismatch_examples':differences,
            'scope':{'actor_ranges':COMMON,'actor_extra':EXTRA,'root_ranges':ROOT+TAIL,'entity_ranges':ENTITY,
                     'excluded':'Guest pointers, untouched bytes outside named ranges, full animation cache/contexts, pad latches (serializer mappings differ), noncurrent roster phase/mask; no floats tolerance, shift, interpolation or inferred callbacks'},
            'equal':not(total or missing_native or missing_original or order_differences or not nc['finish_present'])}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path); parser.add_argument('native', type=Path)
    parser.add_argument('--source-anchor', required=True, type=int); parser.add_argument('--report', required=True, type=Path)
    args=parser.parse_args()
    try:
        need(not args.report.exists() and not args.report.is_symlink(), 'Refusing existing report')
        report=compare(args.original,args.native,args.source_anchor)
        args.report.write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({k:report[k] for k in ['equal','field_mismatch_count','first_mismatch','coverage']}))
        return 0 if report['equal'] else 1
    except (Invalid, KeyError, TypeError, struct.error) as error:
        print('Invalid battle-live comparison: '+str(error),file=sys.stderr); return 2

if __name__=='__main__':
    sys.exit(main())
