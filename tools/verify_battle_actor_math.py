#!/usr/bin/env python3
"""Extract bounded timer/selector fixtures from natural constructor observer v2.

224-byte input and 144-byte output protocol: see battle_actor_math_probe.cpp.
The diagnostic receives explicit RNG results; these are never game inputs.
This verifier compares 5 timer and 3 nonreplacement selector observations only.
It neither repairs global RNG divergence nor proves complete constructors.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

INPUT_SIZE, OUTPUT_SIZE = 224, 144
ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
KNOWN_RAS = {0x15455c, 0x154588, 0x15b794, 0x15c1cc}

def sha(data):
    return hashlib.sha256(data).hexdigest()

def fresh(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as output:
        output.write(data)

def raw(row, key, extent):
    item = row[key]
    data = bytes.fromhex(item['raw_hex'])
    if item['bytes'] != extent or len(data) != extent:
        raise ValueError(f'{key} raw extent')
    return data

def u32(data, at):
    return struct.unpack_from('<I', data, at)[0]

def i32(data, at):
    return struct.unpack_from('<i', data, at)[0]

def i16(data, at):
    return struct.unpack_from('<h', data, at)[0]

def advance(state):
    return (state * 0x41c64e6d + 0x3039) & 0xffffffff

def constructors(rows):
    """Strictly associate ordered entries/completions, registration pairs and RNG."""
    if len(rows) < 3 or rows[0].get('schema') != 'okage-battle-construction-v2':
        raise ValueError('Expected constructor observer v2')
    if rows[0].get('elf_sha256') != ELF_SHA256:
        raise ValueError('Observer ELF identity mismatch')
    finish = rows[-1]
    if finish.get('event') != 'finish' or finish.get('rows') != len(rows)-2:
        raise ValueError('Missing or inconsistent observer finish')
    active, registration, completed = None, None, []
    seen, last_frame, last_rng, registers = set(), -1, None, 0
    for sequence, row in enumerate(rows[1:-1]):
        if row.get('sequence') != sequence or not isinstance(row.get('frame'), int) or row['frame'] < last_frame:
            raise ValueError('Unordered/duplicate observation sequence or frame')
        last_frame = row['frame']
        event = row.get('event')
        if event == 'constructor_entry':
            cid = row['constructor_id']
            if active or cid in seen or row['kind'] not in ('party', 'enemy') or row['rng_entries_since_entry'] != 0:
                raise ValueError('Duplicate/nested constructor entry')
            seen.add(cid)
            expected_pc = 0x153618 if row['kind'] == 'party' else 0x15b580
            if row['pc'] != expected_pc or row['ra'] != 0x115384:
                raise ValueError('Constructor entry address mismatch')
            active = {'entry': row, 'rng': []}
        elif event == 'rng_entry':
            if not active or row['constructor_id'] != active['entry']['constructor_id']:
                raise ValueError('RNG outside corresponding constructor')
            if row['ra'] not in KNOWN_RAS:
                raise ValueError(f'Unsupported/interleaved RNG caller {row["ra"]:#x}')
            if row['pc'] != 0x1f14b0 or not 0 <= row['rng_state'] <= 0xffffffff:
                raise ValueError('RNG entry/state bounds')
            ordinal = row['rng_entry_ordinal']
            if last_rng is not None and ordinal <= last_rng:
                raise ValueError('Duplicate/unordered RNG entry ordinal')
            last_rng = ordinal
            state = active['entry']['rng_state'] if not active['rng'] else advance(active['rng'][-1]['rng_state'])
            if row['rng_state'] != state:
                raise ValueError('Unobserved/interleaved RNG state change')
            active['rng'].append(row)
        elif event in ('registration_entry', 'registration_complete'):
            if row['constructor_id'] != (active['entry']['constructor_id'] if active else None):
                raise ValueError('Registration constructor association')
            if event.endswith('_entry'):
                if registration or row['pc'] != 0x1512f0 or row['registration_index'] != registers or row['registered_count'] != registers or row['return_pc'] not in (0x1537c0, 0x15b7b4, 0x152480):
                    raise ValueError('Duplicate/unordered registration entry')
                registration = row
            else:
                if not registration or row['pc'] != registration['return_pc']:
                    raise ValueError('Unpaired registration completion')
                for key in ('registration_index', 'body_pointer', 'stats_pointer', 'registration_kind', 'battle_root'):
                    if row[key] != registration[key]:
                        raise ValueError('Registration identity changed')
                if row['registered_count'] != registers+1:
                    raise ValueError('Registration completion count')
                registers += 1
                registration = None
        elif event == 'constructor_complete':
            if not active or registration:
                raise ValueError('Unpaired constructor completion')
            entry = active['entry']
            for key in ('constructor_id', 'kind', 'argument', 'child_pointer', 'body_pointer', 'constructor_entry_sp'):
                if row[key] != entry[key]:
                    raise ValueError('Constructor identity changed')
            if row['pc'] != 0x115384 or row['sp'] != entry['sp']:
                raise ValueError('Constructor completion boundary mismatch')
            rng = active['rng']
            state = advance(rng[-1]['rng_state']) if rng else entry['rng_state']
            if row['rng_state'] != state or row['rng_entries_since_entry'] != len(rng):
                raise ValueError('Constructor RNG completion mismatch')
            active['complete'] = row
            completed.append(active)
            active = None
        else:
            raise ValueError(f'Unknown observer event {event}')
    if active or registration or registers != 7 or len(completed) != 5:
        raise ValueError('Incomplete bounded constructor/registration corpus')
    if [x['entry']['kind'] for x in completed] != ['party', 'party', 'enemy', 'enemy', 'enemy']:
        raise ValueError('Unsupported constructor roster/order')
    if [x['entry']['argument'] for x in completed] != [0, 0x60003, 0, 1, 2]:
        raise ValueError('Unsupported constructor source slots/ordinals')
    if [finish.get(k) for k in ('party_completed', 'enemy_completed', 'registrations_completed')] != [2, 3, 7]:
        raise ValueError('Observer finish counts mismatch')
    if last_rng is not None and finish['rng_entries_observed'] < last_rng:
        raise ValueError('Observer final RNG counter mismatch')
    return completed

def extract(rows):
    cases = []
    for actor in constructors(rows):
        entry, end, calls = actor['entry'], actor['complete'], actor['rng']
        party = entry['kind'] == 'party'
        child = raw(end, 'child', 0x3d0 if party else 0x3e0)
        raw(entry, 'child', len(child))
        for row in (entry, end):
            if row['child']['address'] != row['child_pointer'] or row['body_pointer'] != row['child_pointer']+0x40:
                raise ValueError('Child/body pointer association mismatch')
        if party and end['source_slot'] != entry['argument'] >> 16:
            raise ValueError('Party source slot association mismatch')
        context = raw(end, 'entry_context', 0x38)
        if raw(entry, 'entry_context', 0x38)[0x32:0x34] != context[0x32:0x34]:
            raise ValueError('Timer entry flag changed during constructor')
        timer_ra = 0x15455c if party else 0x154588
        expected_ras = [timer_ra] if party else [0x15b794, 0x15c1cc, timer_ra]
        if [x['ra'] for x in calls] != expected_ras:
            raise ValueError('Unsupported RNG helper order/branch or interleaving')
        if i16(context, 0x32) == 1 and party:
            raise ValueError('Observed timer RNG contradicts entry flag')
        def add(op, parameter, kind, entry_half, current, formation, draws, fields, note, initial=None):
            cid = len(cases)
            body = bytearray((i*73 + cid*31 + 17) & 255 for i in range(128))
            if initial:
                for at, data in initial.items():
                    body[16+at:16+at+len(data)] = data
            expected = bytearray(body)
            for at, data in fields.items():
                expected[16+at:16+at+len(data)] = data
            header = [cid, op, parameter & 0xffffffff, kind, entry_half & 0xffffffff,
                      current & 0xffffffff, formation, len(draws)]
            packed = struct.pack('<24I', *header, *draws, *([0]*(16-len(draws)))) + body
            cases.append({'id': cid, 'op': op, 'constructor_id': entry['constructor_id'],
                          'kind': entry['kind'], 'source_slot': end.get('source_slot'),
                          'frame': end['frame'], 'parameter': parameter,
                          'rng_states': [x['rng_state'] for x in calls], 'draws': draws,
                          'consumed': len(draws), 'expected_hex': expected.hex(),
                          'input_hex': packed.hex(), 'comparison_note': note})
        if not party:
            selector_call = calls[1]
            draw = advance(selector_call['rng_state']) & 0x7fffffff
            if ((draw >> 5) & 7) == 0:
                raise ValueError('Random selector replacement outside observed extraction scope')
            selector = child[0x398:0x39a]
            if i16(selector, 0) not in (0, 4):
                raise ValueError('Additional selector RNG branch outside extraction scope')
            add(1, 0, 0, 0, i32(child, 0x3c8), 3, [draw],
                {0x3c: child[0x188:0x18a], 0x40: child[0x18c:0x18e], 0x42: child[0x18e:0x190]},
                'Actual constructor selector outputs; initial selector from owned unit metadata; no random replacement.',
                {0x3c: selector, 0x18: child[0x164:0x166]})
        fields = {offset: child[0x194+offset:0x198+offset] for offset in (0, 4, 8, 16)}
        stan = party and end['source_slot'] == 6
        if stan:
            if fields[16] != bytes(4):
                raise ValueError('Stan constructor timer+10 overwrite not observed')
            fields[16] = struct.pack('<I', 0x3f800000)
        elif fields[16] != struct.pack('<I', 0x3f800000):
            raise ValueError('Timer helper +10 literal mismatch')
        if fields[8] != bytes(4):
            raise ValueError('Timer helper +8 literal mismatch')
        add(0, i16(child, 0x16c) if party else i32(child, 0x378), 0 if party else 1,
            i16(context, 0x32), 0, 0, [advance(calls[-1]['rng_state']) & 0x7fffffff], fields,
            'Timer+0/+4/+8 observed; helper+10 literal1 is source-established before Stan constructor overwrites it with0.'
            if stan else 'All four timer writes observed in constructor completion.')
    return cases

def fixture(cases):
    return b''.join(bytes.fromhex(case['input_hex']) for case in cases)

def compare(cases, inputs, output):
    if inputs != fixture(cases) or not cases or len(cases) > 64:
        raise ValueError('Fixture identity/extent mismatch')
    if len(output) != len(cases)*OUTPUT_SIZE:
        raise ValueError('Native output extent mismatch')
    for i, case in enumerate(cases):
        at = i*OUTPUT_SIZE
        header = struct.unpack_from('<4I', output, at)
        if header != (case['id'], case['op'], case['consumed'], 0):
            raise ValueError(f'Case{i} identity/consumed/reserved mismatch: {header}')
        actual, expected = output[at+16:at+144], bytes.fromhex(case['expected_hex'])
        if actual != expected:
            offset = next(j for j, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1])
            region = f'helper+0x{offset-16:x}' if 16 <= offset < 112 else 'outer canary'
            raise ValueError(f'Case{i} constructor{case["constructor_id"]} op{case["op"]} {region}: '
                             f'byte{offset} expected{expected[offset]:02x} got{actual[offset]:02x}')
    return {'exact': True, 'timer_cases': sum(c['op'] == 0 for c in cases),
            'selector_cases': sum(c['op'] == 1 for c in cases),
            'compared_guarded_bytes': 128*len(cases), 'consumed_counts_exact': True,
            'scope': "Five observed timers and three nonreplacement selector cases; diagnostic explicit draws only."}

def load_reference(path, run_path, root):
    run = json.loads(run_path.read_text())
    if run.get('exit_code') != 0:
        raise ValueError('Original observer run did not succeed')
    command = run['command']
    if command.count('--battle-construction') != 1:
        raise ValueError('Original command observer binding')
    if (root/command[command.index('--battle-construction')+1]).resolve() != path.resolve():
        raise ValueError('Original command observer path mismatch')
    if len(run.get('binary_sha256', '')) != 64:
        raise ValueError('Missing original observer binary identity')
    for key, at in [('source_save_sha256', 3), ('input_sha256', command.index('--pad')+1)]:
        if sha((root/command[at]).read_bytes()) != run[key]:
            raise ValueError(f'Original {key} binding mismatch')
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    return extract(rows)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('generate', 'verify'))
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--reference-run', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True, help='Root resolving retained original command paths')
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--native', type=Path)
    parser.add_argument('--native-run', type=Path)
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists() or args.report.is_symlink():
        raise ValueError('Refusing existing report')
    cases = load_reference(args.reference, args.reference_run, args.root)
    result = {'cases': cases, 'reference_sha256': sha(args.reference.read_bytes()),
              'reference_run_sha256': sha(args.reference_run.read_bytes())}
    if args.command == 'generate':
        fresh(args.input, fixture(cases))
    else:
        if not all((args.native, args.native_run, args.binary)):
            raise ValueError('Verification requires --native --native-run --binary')
        run = json.loads(args.native_run.read_text())
        binary_sha = sha(args.binary.read_bytes())
        if run.get('exit_code') != 0 or run.get('binary_sha256') != binary_sha:
            raise ValueError('Native execution/binary binding mismatch')
        expected_command = [str(p.resolve()) for p in (args.binary, args.input, args.native)]
        if run.get('command') != expected_command or run.get('input_sha256') != sha(args.input.read_bytes()) or run.get('output_sha256') != sha(args.native.read_bytes()):
            raise ValueError('Native command/input/output binding mismatch')
        result.update(compare(cases, args.input.read_bytes(), args.native.read_bytes()))
        result.update(native_sha256=sha(args.native.read_bytes()), native_run_sha256=sha(args.native_run.read_bytes()), binary_sha256=binary_sha)
    result['input_sha256'] = sha(args.input.read_bytes())
    source_root = Path(__file__).resolve().parents[1]
    result['source_sha256'] = {str(p): sha(p.read_bytes()) for p in [Path(__file__).resolve(),
        source_root/'tools/battle_actor_math_probe.cpp', source_root/'src/battle_actor_math.cpp',
        source_root/'include/okage/battle_actor_math.hpp']}
    fresh(args.report, (json.dumps(result, indent=2)+'\n').encode())
    print(json.dumps({k: v for k, v in result.items() if k not in ('cases', 'source_sha256')}, indent=2))

if __name__ == '__main__':
    main()
