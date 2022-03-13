#!/usr/bin/env python3
"""Generate synthetic capture-transform cases and compare raw step records exactly.

Inputs are explicit six-u32 cases; outputs are 80-byte post-step records. This
verifier checks byte agreement only. It does not launch the original or native
program, validate a GS packet, render pixels, or establish capture fidelity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

INPUT_ROW_SIZE = 24
OUTPUT_STEP_SIZE = 80
MAX_CASES = 64
MAX_STEPS_PER_CASE = 2000

STATE_WORDS = ('mode', 'rgba', 'progress', 'vertex_count')

def fixture_cases():
    """Return explicit synthetic cases spanning the recovered transform branches."""
    cases = []
    # Low-byte mode 2, high-mode branches 0/1/4, both field-phase branches.
    for branch in (0, 1, 4):
        for field_nonzero in (0, 1):
            cases.append((branch << 8 | 2, 0x80808080, 0, field_nonzero, 220, 0))

    # 0x10000 - progress == 0x28000 at progress 0xfffe8000; include neighbors.
    for field_nonzero in (0, 1):
        for progress in (0xfffe7fff, 0xfffe8000, 0xfffe8001):
            cases.append((2, 0x80808080, progress, field_nonzero, 1, 0))

    # High mode is negative as a signed word while retaining low-byte mode 2.
    cases.append((0xfffffe02, 0xffffffff, 0x80000000, 1, 2, 0))

    # Signed progress extrema and -1 exercise arithmetic-shift coordinate inputs.
    for progress in (0x7fffffff, 0x80000000, 0xffffffff):
        cases.append((2, 0x80808080, progress, 0, 1, 0))

    # Color/progress raw-word endpoints, including minimum and maximum progress.
    cases.extend((
        (0x102, 0x00000000, 0, 0, 1, 0),
        (0x402, 0xffffffff, 0xffffffff, 1, 1, 0),
    ))

    # Explicitly cover the low-byte switch alternatives around mode 2.
    for mode in (0, 1, 3, 4):
        for field_nonzero in (0, 1):
            cases.append((mode, 0x80808080, 0, field_nonzero, 4, 0))
    return cases

def fixtures():
    data = b''.join(struct.pack('<6I', *case) for case in fixture_cases())
    rows(data)
    return data

def rows(inputs):
    if (not inputs or len(inputs) % INPUT_ROW_SIZE or
            len(inputs) > MAX_CASES * INPUT_ROW_SIZE):
        raise ValueError(f'Battle capture input extent: expected 1..{MAX_CASES} whole 24-byte rows')
    result = []
    for case, offset in enumerate(range(0, len(inputs), INPUT_ROW_SIZE)):
        mode, rgba, progress, field_nonzero, steps, reserved = struct.unpack_from('<6I', inputs, offset)
        if field_nonzero not in (0, 1) or not 1 <= steps <= MAX_STEPS_PER_CASE or reserved:
            raise ValueError(f'Battle capture case {case} header bounds')
        branch = mode >> 8
        result.append({
            'case': case,
            'mode': mode,
            'mode_hex': f'0x{mode:08x}',
            'rgba': rgba,
            'rgba_hex': f'0x{rgba:08x}',
            'progress': progress,
            'progress_hex': f'0x{progress:08x}',
            'field_nonzero': field_nonzero,
            'steps': steps,
            'branch': branch,
        })
    return result

def compare(inputs, original, native):
    cases = rows(inputs)
    total_steps = sum(case['steps'] for case in cases)
    expected_size = total_steps * OUTPUT_STEP_SIZE
    for name, output in (('original', original), ('native', native)):
        if len(output) != expected_size:
            raise ValueError(f'Battle capture {name} output extent: expected {expected_size}, got {len(output)}')

    mismatch = None
    if original != native:
        byte = next(i for i, (a, b) in enumerate(zip(original, native)) if a != b)
        step_index, byte_in_step = divmod(byte, OUTPUT_STEP_SIZE)
        case_index = 0
        first_step = 0
        for case in cases:
            if step_index < first_step + case['steps']:
                break
            first_step += case['steps']
            case_index += 1
        case = cases[case_index]
        step = step_index - first_step
        word, byte_in_word = divmod(byte_in_step, 4)
        original_word = struct.unpack_from('<I', original, step_index * OUTPUT_STEP_SIZE + word * 4)[0]
        native_word = struct.unpack_from('<I', native, step_index * OUTPUT_STEP_SIZE + word * 4)[0]
        if word < 4:
            location = {'kind': 'state', 'field': STATE_WORDS[word]}
        else:
            vertex_word = word - 4
            location = {
                'kind': 'vertex',
                'vertex': vertex_word // 4,
                'coordinate': 'uv' if (vertex_word % 4) < 2 else 'xyz',
                'u32_half': 'low' if vertex_word % 2 == 0 else 'high',
            }
        mismatch = {
            'case': case_index,
            'case_input': {key: case[key] for key in
                           ('mode_hex', 'rgba_hex', 'progress_hex', 'field_nonzero', 'steps')},
            'step': step,
            'global_step': step_index,
            'word': word,
            'byte_in_word': byte_in_word,
            **location,
            'original_word': f'0x{original_word:08x}',
            'native_word': f'0x{native_word:08x}',
        }
    return {
        'case_count': len(cases),
        'total_steps': total_steps,
        'output_step_bytes': OUTPUT_STEP_SIZE,
        'expected_output_bytes': expected_size,
        'compared_bytes': expected_size,
        'exact': mismatch is None,
        'first_difference': mismatch,
        'scope': __doc__.strip(),
        'sha256': {
            name: hashlib.sha256(data).hexdigest()
            for name, data in (('input', inputs), ('original', original), ('native', native))
        },
    }

def write_fresh(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as output:
        output.write(data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    generate = commands.add_parser('generate')
    generate.add_argument('output', type=Path)
    verify = commands.add_parser('verify')
    for name in ('input', 'original', 'native'):
        verify.add_argument(name, type=Path)
    verify.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.command == 'generate':
        data = fixtures()
        write_fresh(args.output, data)
        cases = rows(data)
        result = {'case_count': len(cases), 'total_steps': sum(case['steps'] for case in cases),
                  'input_bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                  'cases': cases, 'scope': __doc__.strip()}
    else:
        inputs = args.input.read_bytes()
        result = compare(inputs, args.original.read_bytes(), args.native.read_bytes())
        result['paths'] = {name: str(getattr(args, name)) for name in ('input', 'original', 'native')}
    encoded = json.dumps(result, indent=2) + '\n'
    if getattr(args, 'report', None):
        write_fresh(args.report, encoded.encode())
    print(encoded, end='')
    if args.command == 'verify' and not result['exact']:
        raise SystemExit(1)

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from error
