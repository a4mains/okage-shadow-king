#!/usr/bin/env python3
"""Audit native sine generation and optionally the existing retail rest-pose corpus.

Reference outputs are comparison data only. The native sine command takes no
input file; the pose command reads owned models, never captured tables or RAM.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

from prepare_assets import ELF_HASH
from verify_event import digest, write_exact
from verify_pose import ABS_TOLERANCE, REL_TOLERANCE

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--pose-corpus', action='store_true')
    args = parser.parse_args()
    if digest(Path('local/extracted/SCUS_971.29')) != ELF_HASH:
        raise ValueError('Different ELF')
    reference = Path('local/retail-sine.f32')
    if digest(reference) != '66fb319284747513a9623c94e0f4e9092e51dbab67204a256d5564dfcc22be5f':
        raise ValueError('Different original sine table')
    args.output.mkdir(parents=True, exist_ok=False)
    native_table = args.output/'native.f32'
    command = [str(args.native), 'sine-table', str(native_table)]
    subprocess.run(command, check=True)
    expected, actual = reference.read_bytes(), native_table.read_bytes()
    if len(actual) != 16384*4 or actual != expected:
        raise ValueError('Native sine table is not exactly the original table')
    report = dict(scope=__doc__, elf_sha256=ELF_HASH,
                  native_binary_sha256=digest(args.native), sine_command=command,
                  exact_sine_words=16384, sine_sha256=digest(native_table))
    if args.pose_corpus:
        original_manifest = Path('local/retail-poses.tsv')
        native_manifest = args.output/'native-poses.tsv'
        command = [str(args.native), 'pose-corpus', 'local/xpf-decoded', str(args.output/'poses')]
        with native_manifest.open('w') as output:
            subprocess.run(command, stdout=output, check=True)
        original = [line.split('\t') for line in original_manifest.read_text().splitlines()]
        native = [line.split('\t') for line in native_manifest.read_text().splitlines()]
        files = sorted(str(p) for p in Path('local/xpf-decoded').rglob('*.xmd'))
        if not files or [r[0] for r in original] != files or [r[0] for r in native] != files:
            raise ValueError('Pose manifest does not cover owned XMD corpus')
        matrices = values = equal = 0
        worst = dict(absolute_error=0)
        reference_hash, native_hash = hashlib.sha256(), hashlib.sha256()
        for (source, source_hash, a, count), (_, b, native_count) in zip(original, native):
            if digest(Path(source)) != source_hash or count != native_count:
                raise ValueError('Pose source hash/count mismatch')
            expected, actual = Path(a).read_bytes(), Path(b).read_bytes()
            if len(expected) != int(count)*64 or len(actual) != len(expected):
                raise ValueError('Pose extent mismatch')
            reference_hash.update(expected);native_hash.update(actual)
            for index, ((x,), (y,)) in enumerate(zip(struct.iter_unpack('<f', expected),
                                                    struct.iter_unpack('<f', actual))):
                error = abs(x-y)
                if not math.isfinite(x) or not math.isfinite(y) or error > ABS_TOLERANCE+REL_TOLERANCE*abs(x):
                    raise ValueError(f'Pose difference: {source} component {index}')
                equal += x == y
                if error > worst['absolute_error']:
                    worst = dict(absolute_error=error, source=source, component=index, original=x, native=y)
            matrices += int(count);values += len(actual)//4
        report['poses'] = dict(command=command, files=len(files), matrices=matrices,
                              components=values, numerically_equal_components=equal, worst=worst,
                              absolute_tolerance=ABS_TOLERANCE, relative_tolerance=REL_TOLERANCE,
                              original_manifest_sha256=digest(original_manifest),
                              native_manifest_sha256=digest(native_manifest),
                              original_concatenated_sha256=reference_hash.hexdigest(),
                              native_concatenated_sha256=native_hash.hexdigest())
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
