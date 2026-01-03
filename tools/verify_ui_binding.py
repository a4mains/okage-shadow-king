#!/usr/bin/env python3
"""Compare only RUW stream binding/reset at the first natural 11fec8 return."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

import verify_battle_hud as hud
import verify_ui_window as win
from verify_battle_interface import parse

Invalid, need, u32 = win.Invalid, win.need, win.u32
SCHEMA = 'okage-native-ui-binding-v1'
SCOPE = 'owned constructors followed only by RUW binding/reset; no UI callbacks or resource clocks'
GROUPS = ((2, 3, 4), (5,))
CALLBACKS = (('first', 0x11fec8), ('second', 0x120ad0))

def raw(value, label, size, address=None):
    need(isinstance(value, dict), f'{label}: missing raw wrapper')
    u32(value.get('bytes'), label + '.bytes')
    u32(value.get('address'), label + '.address')
    return win.raw(value, label, size, address)

def word(data, at):
    return struct.unpack_from('<I', data, at)[0]

def hexbytes(value, size, label):
    need(isinstance(value, str), f'{label}: expected hex string')
    try:
        data = bytes.fromhex(value)
    except ValueError as exc:
        raise Invalid(f'{label}: malformed hex') from exc
    need(len(data) == size and len(value) == size * 2, f'{label}: wrong byte extent')
    return data

def load_reference(path):
    rows = [parse(line) for line in Path(path).read_text().splitlines()]
    need(len(rows) == 10 and all(isinstance(r, dict) for r in rows),
         'reference requires header, eight snapshots, finish')
    header, snapshots, finish = rows[0], rows[1:-1], rows[-1]
    need(header.get('schema') == 'okage-battle-ui-updates-v1' and
         header.get('elf_sha256') == win.ELF_SHA256, 'reference schema/ELF mismatch')
    need(finish.get('event') == 'finish', 'missing reference finish')
    for key, value in [('first_entered', 2), ('first_completed', 2),
                       ('second_entered', 2), ('second_completed', 2), ('snapshots', 8)]:
        need(u32(finish.get(key), 'finish.' + key) == value, 'incomplete reference pairs')
    root = manager = None
    last_frame = 0
    for i, r in enumerate(snapshots):
        kind, pc = CALLBACKS[(i // 2) % 2]
        post = bool(i % 2)
        need(r.get('event') == kind + '_callback_' + ('complete' if post else 'entry') and
             r.get('callback') == hex(pc), 'reference callback order/identity mismatch')
        need(u32(r.get('sequence'), 'sequence') == i and type(r.get('post_callback')) is bool and
             r['post_callback'] == post, 'reference sequence/post flag mismatch')
        frame = u32(r.get('frame'), 'frame')
        need(frame > 0 and frame >= last_frame, 'unordered reference frames')
        last_frame = frame
        for key in ('pc', 'ra', 'sp', 'fcsr', 'root', 'manager', 'caller_return_pc'):
            u32(r.get(key), key)
        need(r['root'] and r['manager'] and r['root'] != r['manager'], 'invalid root/manager')
        if root is None:
            root, manager = r['root'], r['manager']
        need((r['root'], r['manager']) == (root, manager), 'reference owner changed')
        continuation = r['caller_return_pc']
        need(0x100000 <= continuation < 0x2000000 and continuation % 4 == 0 and
             continuation not in (0x11fec8, 0x120ad0), 'invalid caller return PC')
        need(r['ra'] == continuation and r['pc'] == (continuation if post else pc),
             'reference callback/return PC mismatch')
        if post:
            pre = snapshots[i - 1]
            need(all(r[k] == pre[k] for k in ('sp', 'caller_return_pc')),
                 'reference return pairing mismatch')
        body = raw(r.get('battle_root'), 'battle_root', 0x81d0, root)
        need(word(body, 0x8060) == manager, 'root manager relation mismatch')
        m = raw(r.get('manager_raw'), 'manager_raw', 0xef0, manager)
        need(tuple(word(m, at) for at in (0, 4, 8)) == (0x11fec8, 0x120ad0, 0x120b48),
             'manager callback identity mismatch')
        need(word(m, 0xe40) == 2 and word(m, 0xe90) == 0 and word(m, 0xe94) == 1,
             'manager authored layer order mismatch')
        authored = raw(r.get('authored_interface_records_and_terminator'), 'authored', 0xe4, 0x248100)
        for slot, (stream, ca, cb, typ, callback) in enumerate(win.STREAMS):
            at = 0x40 + slot * 0xe0
            layer = raw(r.get(f'layer{slot}'), f'layer{slot}', 0xe0, manager + at)
            need(layer == m[at:at + 0xe0], 'layer snapshot differs from manager')
            need(tuple(word(layer, off) for off in (0, 0x54, 0x5c, 0x60)) ==
                 (slot, stream, callback, typ) and word(layer, 0x50) & 0x40,
                 'inactive or wrong authored layer')
            need(tuple(word(authored, slot * 112 + off) for off in (0, 0x5c, 0x60, 0x64, 0x68)) ==
                 (stream, callback, typ, ca, cb), 'authored layer configuration mismatch')
        if i == 0:
            need(word(m, 0xe4c) == 0 and word(m, 0xe50) == 0,
                 'first callback does not start at both state-zero branches')
    return header, snapshots, finish

def windows(row):
    entries = row.get('controller_windows')
    need(isinstance(entries, list) and len(entries) == 8, 'reference registry requires eight slots')
    registry = raw(row.get('controller_registry'), 'registry', 32, 0x200688)
    need(u32(row.get('controller_count'), 'controller_count') == 2, 'expected two controller windows')
    result = []
    for slot, e in enumerate(entries):
        need(isinstance(e, dict) and u32(e.get('slot'), 'slot') == slot, 'duplicate/reordered registry slot')
        p = u32(e.get('pointer'), 'window pointer')
        need(p == word(registry, slot * 4), 'registry pointer mismatch')
        if slot >= 2:
            need(p == 0 and set(e) == {'slot', 'pointer'}, 'unexpected extra window')
            continue
        need(p != 0, 'missing window owner')
        data = raw(e.get('raw'), 'window', win.WINDOW_BYTES, p)
        need(word(data, 4) == slot and tuple(word(data, at) for at in (0x3c00, 0x3c04)) ==
             win.STREAMS[slot][1:3], 'window slot/capacity mismatch')
        result.append((p, data))
    need(result[0][0] != result[1][0], 'aliased windows')
    return result

def compare(reference, native):
    _header, rows, _finish = reference
    pre, post = rows[:2]
    need(isinstance(native, dict) and set(native) == {'schema', 'scope', 'windows', 'hud'} and
         native['schema'] == SCHEMA and native['scope'] == SCOPE, 'native schema/scope mismatch')
    ns = native['windows']
    need(isinstance(ns, list) and len(ns) == 2, 'native requires two windows')
    all_windows = [windows(r) for r in rows]
    need(all([p for p, _ in w] == [p for p, _ in all_windows[0]] for w in all_windows),
         'reference window owners changed')
    before_widgets = hud._original_widgets(pre)
    after_widgets = hud._original_widgets(post)
    for key in ('raw_pointer', 'runtime_pointer', 'text_pointer'):
        need(pre['ruw_slot0'][key] == post['ruw_slot0'][key], 'RUW owner changed')
    need(isinstance(native['hud'], dict) and native['hud'].get('schema') == hud.NATIVE_SCHEMA and
         native['hud'].get('scope') == hud.NATIVE_SCOPE and
         native['hud'].get('execution') == 'isolated', 'native HUD provenance mismatch')
    need(u32(native['hud'].get('game_mode'), 'HUD game mode') == 3 and
         u32(native['hud'].get('selected'), 'HUD selection') == 0, 'native HUD input identity')
    ngroups = native['hud'].get('groups')
    need(isinstance(ngroups, list) and len(ngroups) == 6, 'native HUD group extent')
    hexbytes(native['hud'].get('vars_hex'), 4096, 'native variables (not compared)')
    for gi, group in enumerate(ngroups):
        need(isinstance(group, list) and len(group) == hud.GROUP_COUNTS[gi], 'native group extent')
        for wi, widget in enumerate(group):
            need(isinstance(widget, dict) and set(widget) == {'words', 'runtime', 'text_hex'}, 'native widget schema')
            words = widget['words']
            need(isinstance(words, list) and len(words) == 17, 'native raw widget extent')
            for v in words: u32(v, 'native widget word')
            need(words[16] == 0, 'native raw guest pointer forbidden')
            orig = after_widgets[gi][wi]
            if orig['runtime'] is None:
                need(widget['runtime'] is None, 'unexpected native runtime owner')
            else:
                need(isinstance(widget['runtime'], list) and len(widget['runtime']) == 10, 'native runtime extent')
                for v in widget['runtime']: u32(v, 'native runtime word')
            if orig['text'] is None:
                need(widget['text_hex'] is None, 'unexpected native text owner')
            else:
                need(b'\0' in hexbytes(widget['text_hex'], 128, 'native text'), 'unterminated native text')

    differences = []
    def equal(label, expected, actual):
        if expected != actual:
            differences.append({'field': label, 'expected': expected, 'actual': actual})
    compared_scalars = compared_stream_words = compared_glyph_bytes = 0
    normalized = []
    per_window = []
    embeds = post.get('embedded_streams')
    need(isinstance(embeds, list) and len(embeds) == 2, 'missing/duplicate embedded streams')
    for slot, n in enumerate(ns):
        need(isinstance(n, dict) and set(n) == {'slot', 'scalar_words', 'commands',
             'glyph_positions_hex', 'glyph_metrics_hex'}, 'native window fields')
        need(u32(n['slot'], 'native slot') == slot, 'native window order')
        scalar = n['scalar_words']
        need(isinstance(scalar, list) and len(scalar) == win.WINDOW_WORDS, 'native scalar extent')
        for v in scalar:
            if v is not None: u32(v, 'native scalar')
        p, b = all_windows[0][slot]; p2, a = all_windows[1][slot]
        need(word(b, 0x2698) == win.STREAMS[slot][0], 'first binding requires authored incoming stream')
        need(word(a, 0x2698) == p + 0x26dc and scalar[0x2698 // 4] is None,
             'active embedded stream pointer relation/native normalization')
        refs = [(g, i) for g in GROUPS[slot] for i in range(hud.GROUP_COUNTS[g])]
        cursor = 4 + 2 * len(refs)
        e = embeds[slot]
        need(isinstance(e, dict) and u32(e.get('slot'), 'embedded slot') == slot and
             u32(e.get('window'), 'embedded owner') == p and
             u32(e.get('active_stream_pointer'), 'embedded pointer') == p + 0x26dc and
             e.get('embedded_stream_active') is True, 'embedded stream identity mismatch')
        need(u32(e.get('cursor'), 'embedded cursor') == cursor and
             u32(e.get('word_count_including_terminator'), 'embedded count') == cursor + 1,
             'embedded stream extent mismatch')
        ew = e.get('words')
        need(isinstance(ew, list) and len(ew) == cursor + 1, 'embedded normalized words extent')
        expected_commands = [{'opcode': 14, 'binding': 'ruw_metrics'}, {'opcode': 6, 'binding': 'ruw_clip'}]
        expected_commands += [{'opcode': 23, 'group': g, 'index': i} for g, i in refs]
        need(isinstance(n['commands'], list) and len(n['commands']) == len(expected_commands), 'native command count')
        for idx, command in enumerate(n['commands']):
            need(isinstance(command, dict) and set(command) == set(expected_commands[idx]), 'native command fields')
            for key, value in command.items():
                if key != 'binding': u32(value, 'native command ' + key)
            need(command == expected_commands[idx], f'native command {slot}:{idx} order/referent mismatch')
        args = {1: 0x200b78, 3: 0x200b88}
        for i, (g, wi) in enumerate(refs):
            off = hud.GROUPS_OFFSET + (hud.GROUP_RECORD_OFFSETS[g] + wi) * 68
            args[5 + 2 * i] = post['ruw_slot0']['raw_pointer'] + off
        for i in range(cursor + 1):
            value = word(a, 0x26dc + i * 4)
            if i in args:
                need(value == args[i], f'original stream pointer {slot}:{i} mismatch')
                need(scalar[0x26dc // 4 + i] is None, 'native pointer argument must be unknown')
                expected_observed = value if i < 4 else {'raw_widget_offset': value - post['ruw_slot0']['raw_pointer']}
                need(ew[i] == expected_observed, 'observer normalized pointer disagrees with raw window')
                if isinstance(ew[i], dict): u32(ew[i]['raw_widget_offset'], 'widget offset')
                else: u32(ew[i], 'prefix pointer')
                normalized.append({'slot': slot, 'offset': 0x26dc + i * 4, 'role': 'stream argument'})
            else:
                expected = 0 if i == cursor else (14 if i == 0 else 6 if i == 2 else 23)
                need(u32(ew[i], 'stream opcode') == value == expected, 'original stream opcode/terminator mismatch')
                equal(f'window{slot}.stream_word{i}', expected, scalar[0x26dc // 4 + i])
                compared_stream_words += 1
        normalized.append({'slot': slot, 'offset': 0x2698, 'role': 'active embedded stream'})
        targets = {0x2edc: cursor, 0x3bec: 2, 0x3be4: 0xffffffff,
                   0x268: word(b, 0x26c), 0x258: word(b, 0x250), 0x254: word(b, 0x250),
                   0x1900: (word(b, 0x1900) & 0xfffff6c5) | 1}
        targets.update({at: 0 for at in (0xcc0, 0xcc4, 0xce8, 0x264, 0x260, 0xcd0)})
        ca, cb = win.STREAMS[slot][1:3]
        for i in range(32):
            at = 0x3c08 + i * 4
            targets[at] = word(b, at) | (5 if i < ca else 0) | (8 if i < cb else 0)
        for at, expected in targets.items():
            equal(f'original.window{slot}.reset@{at:x}', expected, word(a, at))
            equal(f'native.window{slot}.reset@{at:x}', expected, scalar[at // 4])
            compared_scalars += 1
        extent = (ca + 1) * 256
        need(u32(e.get('glyph_buffer_capacity'), 'glyph capacity') == ca and
             u32(e.get('glyph_buffer_allocation_count'), 'glyph allocation count') == ca + 1 and
             u32(e.get('glyph_buffer_extent_bytes'), 'glyph bytes') == extent, 'glyph capture extent')
        for name, ptr_at in [('glyph_positions', 0x650), ('glyph_metrics', 0x654)]:
            ptr = u32(e.get(name + '_pointer'), 'glyph pointer')
            need(ptr and ptr == word(a, ptr_at) == word(b, ptr_at), 'glyph owner relation changed')
            actual = raw(e.get(name), name, extent, ptr)
            got = hexbytes(n[name + '_hex'], extent, 'native.' + name)
            expected = bytearray(extent)
            if name == 'glyph_metrics':
                for i in range(ca * 16): struct.pack_into('<I', expected, i * 16 + 8, 0xbf800000)
            for label, data in [('original', actual), ('native', got)]:
                bad = [i for i, (x, y) in enumerate(zip(expected, data)) if x != y]
                if bad: differences.append({'field': f'{label}.window{slot}.{name}', 'different_bytes': len(bad),
                                            'first_offset': bad[0], 'expected': expected[bad[0]], 'actual': data[bad[0]]})
            compared_glyph_bytes += extent
        per_window.append({'slot': slot, 'widget_commands': len(refs), 'cursor': cursor,
                           'glyph_bytes': extent * 2, 'nonbinding_opacity_offsets': [0x270, 0x274, 0x278, 0x27c],
                           'nonbinding_transform_offsets': ([0x4f4] + [at + i * 4 for at in (0x2f0, 0x520, 0x3f0, 0x550) for i in range(3)]) if slot else []})

    eligible = changed = primary_words = 0
    for g, (bg, ag) in enumerate(zip(before_widgets, after_widgets)):
        for i, (b, a) in enumerate(zip(bg, ag)):
            equal(f'original.widget{g}:{i}.raw_preserved', list(b['words']), list(a['words']))
            n = ngroups[g][i]
            if g < 2: continue
            equal(f'native.widget{g}:{i}.binding_input', list(b['words'][:16]), n['words'][:16])
            if b['runtime'] is None: continue
            expected = list(b['runtime'])
            if b['words'][9] == 1 and b['words'][8] != 0xffffffff:
                eligible += 1
                target = b['words'][3 if b['words'][10] else 2]
                changed += target != expected[4]
                expected[4] = target
            equal(f'original.widget{g}:{i}.runtime', expected, list(a['runtime']))
            equal(f'original.widget{g}:{i}.text_preserved', None if b['text'] is None else b['text'].hex(),
                  None if a['text'] is None else a['text'].hex())
            equal(f'native.widget{g}:{i}.primary', expected[4], n['runtime'][4])
            primary_words += 1
    return {'schema': 'okage-ui-binding-verification-v1', 'status': 'fail' if differences else 'pass',
            'scope': 'binding/reset only at first 11fec8 return; no native callback/clock/layout execution',
            'reference_elf_sha256': _header['elf_sha256'],
            'reference_pair_frames': [[r['frame'] for r in rows[i:i + 2]] for i in range(0, 8, 2)],
            'reference_fcsr': [r['fcsr'] for r in rows], 'fcsr_compared_to_native': False,
            'compared_reset_and_flag_words': compared_scalars, 'compared_stream_opcode_words': compared_stream_words,
            'normalized_pointer_relations': normalized, 'compared_glyph_bytes': compared_glyph_bytes,
            'compared_primary_color_words': primary_words, 'eligible_color_writes': eligible,
            'source_expected_changed_color_values': changed, 'windows': per_window,
            'difference_count': len(differences), 'first_difference': differences[0] if differences else None,
            'differences': differences, 'limitations': [
                'Only the first dispatcher pair supplies comparison values; all four pairs are structurally validated.',
                'Opacity and second-window transform are controller writes, not binding output; other window words are outside this comparison.',
                'Glyph extents are source allocation-request extents; observer does not capture allocator headers.',
                'Shared 200a88 and command-referent contents, sprite/layout caches, full HUD output, RNG, variables, rendering and callback behavior are not compared.',
                'Input file hashes bind this comparison. External run exit status, native binary and owned-asset provenance require the separate run manifests; this tool does not certify them.',
                'Idempotent color values cannot prove an original write executed; no original state is a native runtime input.']}

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('original_jsonl'); p.add_argument('native_json'); p.add_argument('--output', required=True)
    args = p.parse_args(argv); output = Path(args.output)
    try:
        need(not output.exists() and not output.is_symlink(), 'refusing existing output')
        reference = load_reference(args.original_jsonl)
        report = compare(reference, parse(Path(args.native_json).read_text()))
        report['inputs'] = {name: {'path': str(Path(path).resolve()), 'sha256': hashlib.sha256(Path(path).read_bytes()).hexdigest()}
                            for name, path in [('original', args.original_jsonl), ('native', args.native_json)]}
        report['verifier_sha256'] = {Path(m.__file__).name: hashlib.sha256(Path(m.__file__).read_bytes()).hexdigest()
                                   for m in (hud, win, sys.modules['verify_battle_interface'])}
        report['verifier_sha256'][Path(__file__).name] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        with output.open('x') as f: json.dump(report, f, indent=2); f.write('\n')
        print(f"{report['status']}: {report['difference_count']} differences; {report['compared_glyph_bytes']} glyph bytes")
        return 0 if report['status'] == 'pass' else 1
    except (Invalid, OSError, ValueError, KeyError, TypeError) as exc:
        print(f'UI binding verification error: {exc}', file=sys.stderr); return 2

if __name__ == '__main__':
    raise SystemExit(main())
