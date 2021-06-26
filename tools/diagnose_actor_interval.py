#!/usr/bin/env python3
"""Compare actor state/channels at fixed original field-manager boundaries.

Reports differences without shifting frames or fitting tolerances. Original
positive-instance observations precede the update, so each is compared with
the preceding completed native frame. Texture selection, visibility, evaluated
world poses, pixels and audio are outside this comparison.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from prepare_assets import ELF_HASH
from verify_message_opening import require

def records(path, *, native):
    """Channel counts can change when an alias is reused in a later scene."""
    result = []
    with path.open('rb') as stream:
        while True:
            header = stream.read(32)
            if not header:
                break
            require(len(header) == 32, 'Truncated actor header')
            header = struct.unpack('<8I', header)
            require(4 <= header[5] <= 5120, 'Actor channel bounds')
            # Original word6 is a motion request ID, not an array extent.
            textures = header[6] if native else 0
            require(textures <= 4096, 'Actor texture extent')
            words = header[5] * 4 + textures
            payload = stream.read(words * 4)
            require(len(payload) == words * 4, 'Truncated actor payload')
            result.append(header + struct.unpack('<' + str(words) + 'I', payload))
    return result

def compare(original, native, *, instance, anchor, frames):
    require(anchor > 1 and frames > 0, 'Invalid actor comparison interval')
    require(all(r[4] == instance and r[7] in (0x133a98, 0x1b7a68)
                for r in original), 'Unexpected original actor identity/hook')
    original = [r for r in original if r[7] == 0x133a98]
    require([r[0] for r in original] == list(range(frames)),
            'Missing, repeated or unordered original actor frame')
    native = [r for r in native if anchor - 1 <= r[0] < anchor + frames - 1]
    require([r[0] for r in native] == list(range(anchor - 1, anchor + frames - 1)),
            'Missing, repeated or unordered native actor frame')
    keys = ('field_pc', 'actor_pc', 'actor_flags', 'root_words', 'other_channel_words')
    differences = {k: [] for k in keys}
    counts = {k: 0 for k in keys}
    channel_words = 0
    for r, n in zip(original, native):
        require(r[5] == n[5], 'Original/native channel count differs')
        end = 8 + 4 * r[5]
        slices = (slice(1, 2), slice(2, 3), slice(3, 4), slice(8, 20), slice(20, end))
        channel_words += 4 * r[5]
        for key, part in zip(keys, slices):
            require(len(r[part]) == len(n[part]), 'Actor comparison extent')
            count = sum(a != b for a, b in zip(r[part], n[part]))
            if count:
                differences[key].append(r[0])
                counts[key] += count
    return dict(frames=frames, channel_words=channel_words,
                difference_frames=differences, different_words=counts,
                all_compared_words_equal=not any(differences.values()))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--reference-trace', type=Path, required=True)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--instance', type=int, required=True)
    parser.add_argument('--alias', type=int, required=True)
    parser.add_argument('--first-frame', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), 'Refusing existing actor report')
    require(0 <= args.instance < 2048, 'Expected positive original map instance')
    original_manifest = Path(str(args.reference) + '.run.json')
    native_manifest = Path(str(args.capture.with_suffix('')) + '.run.json')
    original_run = json.loads(original_manifest.read_text())
    native_run = json.loads(native_manifest.read_text())
    require(original_run['exit_code'] == native_run['exit_code'] == 0, 'Incomplete run')
    command = original_run['command']
    require(command[command.index('--actor') + 1] == str(args.reference_trace) and
            int(command[command.index('--actor-alias') + 1]) == args.instance,
            'Original actor command differs')
    native_command = native_run['command']
    require(native_command[native_command.index('--capture') + 1] == str(args.capture),
            'Native capture command differs')
    frames = int(command[2])
    log = Path(str(args.reference) + '.log')
    source = Path(command[3])
    log_text = log.read_text()
    require(f'Loaded reference state {source}' in log_text and
            f'Stopped: vblanks={frames} exit_request=0' in log_text,
            'Original did not complete the interval')
    elf = Path('local/extracted/SCUS_971.29')
    require(hashlib.sha256(elf.read_bytes()).hexdigest() == ELF_HASH, 'Different original ELF')
    suffix = '.actor.bin' if args.alias == -8 else f'.actor{args.alias}.bin'
    native_trace = Path(str(args.capture) + suffix)
    result = compare(records(args.reference_trace, native=False),
                     records(native_trace, native=True), instance=args.instance,
                     anchor=args.first_frame, frames=frames)
    artifacts = [original_manifest, native_manifest, args.reference_trace, native_trace, log, source, elf]
    report = dict(scope=__doc__, elf_sha256=ELF_HASH, instance=args.instance,
                  alias=args.alias, first_native_frame=args.first_frame,
                  original_run=original_run, native_run=native_run, **result,
                  sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts})
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'difference_frames'}))

if __name__ == '__main__':
    main()
