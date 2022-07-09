#!/usr/bin/env python3
"""Compare the bounded BattleInterface constructor state with a natural observer."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

import verify_ui_window as window

Invalid = window.Invalid
need = window.need
u32 = window.u32
NATIVE_SCHEMA = 'okage-native-battle-interface-v1'
SCOPE = "11f5f8 constructor before child activation; no callbacks or rendering"
OUTPUT_SCHEMA = 'okage-battle-interface-verification-v1'
MANAGER_BYTES = 0xef0
MANAGER_WORDS = MANAGER_BYTES // 4
NORMALIZED_OFFSETS = (0, 4, 8, 12, 16, 20, 24, 0x44, 0x94, 0x9c, 0x124, 0x174, 0x17c)
CALLBACKS = (0x11fec8, 0x120ad0, 0x120b48)

def strict_object(pairs):
    result = {}
    for key, value in pairs:
        need(key not in result, f'duplicate JSON object key: {key}')
        result[key] = value
    return result

def parse(text):
    return json.loads(text, object_pairs_hook=strict_object,
                      parse_constant=lambda value: (_ for _ in ()).throw(Invalid(f'invalid JSON constant: {value}')))

def load_reference(path):
    for line in Path(path).read_text().splitlines():
        parse(line)
    return window.load_reference(path)

def raw(row, key, size, address):
    value = row.get(key)
    need(isinstance(value, dict), f'{key}: missing raw object')
    u32(value.get('address'), f'{key}.address')
    u32(value.get('bytes'), f'{key}.bytes')
    return window.raw(value, key, size, address)

def word(data, offset):
    return struct.unpack_from('<I', data, offset)[0]

def compare(reference, native):
    _header, entry, complete, _finish, root = reference
    need(isinstance(native, dict) and set(native) == {
        'schema', 'frame', 'scope', 'manager_words', 'current_stream',
        'stream_cursor', 'atlas_dirty', 'windows'}, 'native interface fields mismatch')
    need(native['schema'] == NATIVE_SCHEMA and native['scope'] == SCOPE,
         'native interface schema/scope mismatch')
    native_frame = u32(native['frame'], 'native.frame')
    need(native_frame > 0, 'native frame must be positive')
    need(type(native['atlas_dirty']) is bool, 'native atlas_dirty must be boolean')
    native_words = native['manager_words']
    need(isinstance(native_words, list) and len(native_words) == MANAGER_WORDS,
         f'native manager requires exactly {MANAGER_WORDS} words')
    for i, value in enumerate(native_words):
        u32(value, f'native.manager_words[{i}]')

    manager_pointer = u32(complete['manager'], 'complete.manager')
    need(root != 0 and manager_pointer != root, 'invalid manager/root identity')
    original = raw(complete, 'manager_raw', MANAGER_BYTES, manager_pointer)
    roots = [raw(row, 'battle_root', 0x81d0, root) for row in (entry, complete)]
    children = [tuple(word(data, at) for at in (0x8064, 0x8068, 0x806c, 0x8070)) for data in roots]
    need(children[0] == children[1] and len(set(children[0])) == 4 and all(children[0]),
         'original stage/camera/party/enemy owner identities changed or overlap')
    need(root not in children[0] and manager_pointer not in children[0], 'original manager/root overlaps child owner')
    need(all(word(data, 0x18) == children[0][0] and word(data, 0x8060) == 0 for data in roots),
         'original root child head or pre-store UI pointer mismatch')
    need(word(roots[0], 0x1c) == word(roots[1], 0x1c), 'original parent flags changed')
    need(word(original, 0x1c) == (word(roots[0], 0x1c) & 0x18000000),
         'original manager flags do not match inherited source mask')

    normalizations = []

    def binding(offset, expected, normalized, role):
        need(word(original, offset) == expected, f'original {role} mismatch at {offset:#x}')
        need(native_words[offset // 4] == normalized, f'native {role} binding mismatch at {offset:#x}')
        normalizations.append({'offset': offset, 'role': role, 'original': expected, 'native': normalized})

    for offset, callback, role in zip((0, 4, 8), CALLBACKS, ('first callback', 'second callback', 'destructor')):
        binding(offset, callback, 0, role + ' source identity/native unimplemented placeholder')
    binding(0xc, children[0][3], 0, 'previous sibling is root enemy owner')
    binding(0x10, 0, 0, 'next sibling null')
    binding(0x14, root, 0, 'parent owner')
    binding(0x18, 0, 0, 'child link null')
    source_texture = u32(complete['shared_globals'].get('battle_ui_texture'), 'complete.battle_ui_texture')
    need(source_texture != 0xffffffff and source_texture == u32(entry['shared_globals'].get('battle_ui_texture'),
         'entry.battle_ui_texture'), 'original texture owner changed or invalid')
    authored = raw(complete, 'authored_interface_records_and_terminator', 0xe4, 0x248100)
    for i, (stream, _a, _b, _type, callback) in enumerate(window.STREAMS):
        base = 0x40 + i * 0xe0
        raw(complete, f'layer{i}', 0xe0, manager_pointer + base)
        need(word(authored, i * 112 + 0x58) == (0x163fb8, 0x168228)[i],
             f'authored layer{i} constructor identity mismatch')
        binding(base + 4, source_texture, 1, f'layer{i} shared texture owner')
        binding(base + 0x54, stream, i + 1, f'layer{i} authored stream')
        binding(base + 0x5c, callback, i + 1, f'layer{i} authored update callback')
        need(word(original, base + 0x50) == 0x50 and word(original, base + 0xd4) == 0,
             f'original layer{i} is not the constructed pre-activation state')
        controller = complete['controller_windows'][i]
        data = raw(controller, 'raw', window.WINDOW_BYTES, controller['pointer'])
        need(word(data, 8) == source_texture, f'original window{i} texture owner mismatch')
        need(word(data, 0x2698) == stream and word(data, 0x3bf0) == stream,
             f'original window{i} persistent stream identity mismatch')
    need(set(item['offset'] for item in normalizations) == set(NORMALIZED_OFFSETS),
         'internal manager normalization coverage mismatch')

    stream_state = raw(complete, 'controller_stream_state', 8, 0x1f9ca0)
    need(word(stream_state, 0) == window.STREAMS[1][0] and word(stream_state, 4) == 4,
         'original final stream builder/cursor outside constructor scope')
    current = u32(native['current_stream'], 'native.current_stream')
    cursor = u32(native['stream_cursor'], 'native.stream_cursor')
    differences = []
    for offset in range(0, MANAGER_BYTES, 4):
        if offset in NORMALIZED_OFFSETS:
            continue
        expected, actual = word(original, offset), native_words[offset // 4]
        if expected != actual:
            differences.append({'component': 'manager', 'offset': offset, 'expected': expected, 'actual': actual})
    for field, expected, actual in (('current_stream', 2, current), ('stream_cursor', 4, cursor)):
        if expected != actual:
            differences.append({'component': 'builder', 'field': field, 'expected': expected, 'actual': actual})

    windows = window.compare(reference, {
        'schema': window.NATIVE_SCHEMA,
        'scope': "198e38 and window configuration only; empty incoming streams and atlas, slots0/1; no layer constructors",
        'windows': native['windows']})
    differences.extend({'component': 'window', **item} for item in windows['differences'])
    return {
        'schema': OUTPUT_SCHEMA, 'status': 'fail' if differences else 'pass',
        'scope': "paired constructor-state comparison; native/original frames are reported separately, not temporally aligned",
        'native_frame': native_frame, 'reference_frame': complete['frame'],
        'reference_entry_pc': entry['pc'], 'reference_completion_pc': complete['pc'],
        'reference_root': root, 'reference_manager': manager_pointer,
        'manager': {'total_words': MANAGER_WORDS, 'compared_words': MANAGER_WORDS - len(NORMALIZED_OFFSETS),
                    'normalized_words': len(normalizations), 'normalizations': normalizations},
        'windows': {'compared_words': windows['compared_words'], 'per_slot': windows['windows'],
                    'excluded_offsets_hex': windows['excluded_offsets_hex']},
        'compared_words': MANAGER_WORDS - len(NORMALIZED_OFFSETS) + windows['compared_words'],
        'builder': {'compared_fields': 2, 'original_stream': word(stream_state, 0),
                    'native_stream': current, 'original_cursor': 4, 'native_cursor': cursor},
        'atlas_dirty': {'native': native['atlas_dirty'], 'original': None, 'compared': False},
        'difference_count': len(differences), 'first_difference': differences[0] if differences else None,
        'differences': differences,
        'unsupported': [
            'Source callback identities are checked against native zero placeholders; this is not callback implementation or execution parity.',
            'Source task links are checked against captured root owner fields and source construction order; native links are host-owned zero placeholders. Enemy sibling-link memory was not captured.',
            'Native texture ID1 denotes the shared binding, not a guest GS handle; resource bytes, upload and rendering are outside this output.',
            'Native stream contents are not emitted; only stream identity and builder cursor are compared.',
            'Original 200530 atlas-dirty is not captured; native atlas_dirty is type-validated only.',
            'Window heap-pointer/accounting fields and uninitialized W53c use the existing exact exclusion mask; auxiliary allocation contents are not compared.',
            'No RUW, text, HUD shared-state, RNG, full-frame, timing, UI activation or gameplay readiness parity is claimed.']}

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', required=True)
    parser.add_argument('--native', required=True)
    parser.add_argument('--output', required=True, help='fresh report path; existing paths are never replaced')
    args = parser.parse_args(argv)
    output = Path(args.output)
    try:
        need(not output.exists() and not output.is_symlink(), f'refusing existing output: {output}')
        need(output.resolve() not in {Path(args.reference).resolve(), Path(args.native).resolve()}, 'output aliases input')
        reference = load_reference(args.reference)
        native = parse(Path(args.native).read_text())
        report = compare(reference, native)
        report['inputs'] = {label: {'path': str(Path(path).resolve()), 'sha256': sha(path)}
                            for label, path in (('reference', args.reference), ('native', args.native))}
        report['elf_sha256'] = window.ELF_SHA256
        report['tool_sha256'] = {Path(path).name: sha(path) for path in (__file__, window.__file__)}
        with output.open('x') as stream:
            json.dump(report, stream, indent=2)
            stream.write('\n')
    except (Invalid, OSError, ValueError, struct.error) as exc:
        print(f'verify-battle-interface: {exc}', file=sys.stderr)
        return 2
    print(f"{report['status']}: {report['compared_words']} scalar words plus 13 normalized manager words and 2 builder fields; {report['difference_count']} differences")
    return 0 if report['status'] == 'pass' else 1

if __name__ == '__main__':
    raise SystemExit(main())
