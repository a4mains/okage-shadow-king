#!/usr/bin/env python3
"""Verify the bounded native MAM walk/turn integration, retaining startup mismatches."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from verify_animation import compare, sha

def run(*args):
    subprocess.run([str(a) for a in args], check=True)

def compare_scene(reference, native, release):
    retail = list(struct.iter_unpack('<656I', reference.read_bytes()))
    rows = list(struct.iter_unpack('<671I', native.read_bytes()))
    if len(retail) != 220 or [r[0] for r in retail] != list(range(220)):
        raise ValueError('Incomplete original actor capture')
    if len(rows) != 360 or [r[0] for r in rows] != list(range(1, 361)):
        raise ValueError('Incomplete native actor capture')
    if any(r[5] != 162 or r[7] not in (0x1856f8, 0x185fb0) for r in retail):
        raise ValueError('Original actor observation scope mismatch')
    if any(r[5:7] != (162, 15) for r in rows):
        raise ValueError('Native actor layout mismatch')
    original_release = next(r[0] for r in retail if r[1] == 0xc10)
    offset = release - 1 - original_release
    if offset < 0 or offset + len(retail) > len(rows):
        raise ValueError('Actor alignment bounds')
    early_mismatches = []
    for i, r in enumerate(retail):
        n = rows[offset + i]
        if n[1] != r[1] or n[8:20] != r[8:20]:
            raise ValueError('Event/root mismatch at original frame %d' % i)
        if i >= original_release and n[2:4] != r[2:4]:
            raise ValueError('Actor script state mismatch at original frame %d' % i)
        differing = sum(a != b for a, b in zip(r[8:656], n[8:656]))
        if differing:
            early_mismatches.append({'reference_frame': i, 'different_channel_words': differing})
        if i >= 97 and differing:
            raise ValueError('Actor channels differ after initial transition at %d' % i)
    if rows[-1][1] != 0xc48 or rows[-1][4] != 1:
        raise ValueError('Unexpected final event/motion')
    return {
        'reference_frames': len(retail), 'native_frames': len(rows),
        'reference_release_frame': original_release, 'native_release_frame': release,
        'native_row_offset': offset, 'event_pc_exact_frames': len(retail),
        'root_transform_exact_frames': len(retail), 'root_words_per_frame': 12,
        'actor_script_exact_from_reference_frame': original_release,
        'all_648_channel_words_exact_from_reference_frame': 97,
        'exact_channel_frames': len(retail) - 97,
        'earlier_channel_mismatches': early_mismatches,
        'full_trace_equivalent': not early_mismatches,
        'reference_sha256': sha(reference), 'native_sha256': sha(native),
    }

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=Path('build/native'))
    p.add_argument('--output', type=Path, default=Path('local/actor-validation-final'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    native = args.build/'okage-assets'
    app = args.build/'okage-native.app/Contents/MacOS/okage-native'
    base = Path('local/xpf-decoded/FLDDATA/OBJ/MAM.XPF')
    script = Path('local/xpf-decoded/FLDDATA/EVENT/EVDATA.XPF/e_home1c.xmt')
    clips = []
    for entry in (1, 2):
        out = args.output/('mam-%d.native.bin' % entry)
        reference = Path('local/actor-validation')/('mam-%d.retail.bin' % entry)
        run(native, 'animation', base/'a.xmt', 162, entry, 180, out, base/'a.xmd', base/'a.xta')
        result = compare(reference, out, 162, 79, 180, 15)
        if not result['passed']:
            raise ValueError('Original MAM clip mismatch: %d' % entry)
        clips.append(dict(result, entry=entry, native_sha256=sha(out), reference_sha256=sha(reference)))
    capture = args.output/'frame-360.png'
    run(app, 'local/font-input/CMNDATA/FONT', 'local/text-input/FLDDATA/MES/FIELD.PME',
        '--scene', 'local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF/a.xmp',
        '--event', script, '--actor', base/'a.xmd', '--frames', 360,
        '--press', '100,140,180,220,260,300,340', '--capture', capture)
    state = json.loads(Path(str(capture)+'.json').read_text())
    integration = compare_scene(Path('local/reference-actor-820-v2.bin'),
                                Path(str(capture)+'.actor.bin'), state['wait_release_frame'])
    report = {
        'scope': 'Native HOME1C diagnostic actor -8 segment; not cold startup or a playable opening',
        'native_tool_sha256': sha(native), 'native_app_sha256': sha(app),
        'elf_sha256': sha(Path('local/extracted/SCUS_971.29')),
        'script_sha256': sha(script),
        'actor_assets': {name: sha(base/name) for name in ('a.xmd', 'a.xmt', 'a.xta')},
        'isolated_clips': clips, 'integration': integration, 'native_state': state,
        'bounded_checks_passed': True,
        'limits': ['Earlier idle phase and first nine walking-transition frames differ; full trace equivalence is false',
                   'Initial actor binding and scene start are explicit harness assumptions',
                   'Full-game trace does not observe texture selection; isolated clips do',
                   'Dialogue-close wait at 0xc48 remains pending; no message-absent flag is forced',
                   'No native audio, other actors, shadows, effects, faithful lighting/windows, battles or saves'],
    }
    (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k: v for k, v in integration.items() if k != 'earlier_channel_mismatches'}, indent=2))

if __name__ == '__main__':
    main()
