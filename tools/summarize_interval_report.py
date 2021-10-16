#!/usr/bin/env python3
"""Summarize first divergence in an existing fixed-anchor interval report.

This consumes the JSON from diagnose_home1b_interval.py. It does not run either
implementation, alter anchors, or compare image/audio streams.
"""

import argparse
import hashlib
import json
from pathlib import Path

def _integer(value, name, *, minimum=0):
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ValueError(f'{name} must be an integer >= {minimum}')
    return value

def _frame_list(value, name, total):
    if not isinstance(value, list):
        raise ValueError(f'{name} must be a list')
    frames = []
    previous = -1
    for index, frame in enumerate(value):
        frame = _integer(frame, f'{name}[{index}]')
        if frame >= total:
            raise ValueError(f'{name}[{index}]={frame} is outside 0..{total - 1}')
        if frame <= previous:
            raise ValueError(f'{name} must be strictly increasing without duplicates')
        frames.append(frame)
        previous = frame
    return frames

def _ranges(frames, first_native_frame):
    result = []
    for frame in frames:
        if result and frame == result[-1]['original_end_frame'] + 1:
            result[-1]['original_end_frame'] = frame
            result[-1]['native_tick_end'] = first_native_frame + frame
            result[-1]['frames'] += 1
        else:
            result.append({
                'original_start_frame': frame,
                'original_end_frame': frame,
                'native_tick_start': first_native_frame + frame,
                'native_tick_end': first_native_frame + frame,
                'frames': 1,
            })
    return result

def summarize(report, *, source_path='<input>', source_sha256=None):
    """Validate and summarize one diagnose_home1b_interval.py report."""
    if not isinstance(report, dict):
        raise ValueError('input report must be a JSON object')
    total = _integer(report.get('original_frames'), 'original_frames', minimum=1)
    anchor = _integer(report.get('first_native_frame'), 'first_native_frame', minimum=2)
    controller_samples = _integer(report.get('controller_samples'), 'controller_samples')
    if controller_samples > total:
        raise ValueError('controller_samples exceeds original_frames')

    differences = report.get('difference_frames')
    if not isinstance(differences, dict) or not differences:
        raise ValueError('difference_frames must be a nonempty object of frame lists')
    category_frames = {
        category: _frame_list(frames, f'difference_frames.{category}', total)
        for category, frames in differences.items()
        if isinstance(category, str) and category
    }
    if len(category_frames) != len(differences):
        raise ValueError('difference_frames category names must be nonempty strings')
    for category in ('controller', 'choice'):
        if category in category_frames and len(category_frames[category]) > controller_samples:
            raise ValueError(f'difference_frames.{category} exceeds controller_samples')

    missing = _frame_list(report.get('missing_original_player_frames'),
                          'missing_original_player_frames', total)
    missing_manager = _frame_list(report.get('missing_original_manager_frames', []),
                                  'missing_original_manager_frames', total)
    missing_camera = _frame_list(report.get('missing_original_camera_frames', []),
                                 'missing_original_camera_frames', total)
    if set(missing) & set(missing_manager):
        raise ValueError('missing_original_player_frames requires a present manager observation')
    missing_by_kind = dict(manager=missing_manager, camera=missing_camera, player=missing)
    all_missing = sorted({f for frames in missing_by_kind.values() for f in frames})
    missing_by_channel = dict(manager=missing_manager, camera=missing_camera,
                              player=sorted(set(missing_manager + missing)))
    coverage = {name: dict(expected_frames=total, observed_frames=total-len(frames))
                for name, frames in missing_by_channel.items()}
    complete_coverage = not all_missing
    if 'complete_original_observation_coverage' in report:
        claimed = report['complete_original_observation_coverage']
        if not isinstance(claimed, bool) or claimed != complete_coverage:
            raise ValueError('complete_original_observation_coverage contradicts missing observations')
    if 'original_observation_coverage' in report:
        claimed = report['original_observation_coverage']
        if not isinstance(claimed, dict) or set(claimed) != set(coverage):
            raise ValueError('original_observation_coverage must contain manager, camera, player')
        for name, expected in coverage.items():
            if not isinstance(claimed[name], dict) or set(claimed[name]) != set(expected):
                raise ValueError(f'original_observation_coverage.{name} has invalid fields')
            for field, value in expected.items():
                if _integer(claimed[name][field], f'original_observation_coverage.{name}.{field}') != value:
                    raise ValueError(f'original_observation_coverage.{name}.{field} contradicts missing observations')
    for category, frames in category_frames.items():
        channel = ('manager' if category in ('manager_running_pc_gate', 'pad_latch', 'direction_latch')
                   else 'player' if category in ('player_mask', 'hero_facing_w', 'hero_position')
                   else 'camera' if category.startswith('camera_') else None)
        if channel and set(frames) & set(missing_by_channel[channel]):
            raise ValueError(f'difference_frames.{category} includes a missing observation')
    if 'unblended_target_compared_frames' in report:
        targets = _frame_list(report['unblended_target_compared_frames'],
                             'unblended_target_compared_frames', total)
        if targets and report.get('unblended_target_check') is False:
            raise ValueError('unblended_target_compared_frames contradicts disabled target check')
        if any(f == 0 or f in missing_manager or f in missing_camera or f-1 in missing_camera
               for f in targets):
            raise ValueError('unblended_target_compared_frames requires current manager/camera and prior camera')
        if not set(category_frames.get('camera_unblended_target', [])) <= set(targets):
            raise ValueError('camera_unblended_target differences require compared target frames')
    union = sorted({frame for frames in category_frames.values() for frame in frames})
    first_difference_frame = union[0] if union else None
    first_categories = ([name for name, frames in category_frames.items()
                         if first_difference_frame in frames]
                        if first_difference_frame is not None else [])

    agrees = report.get('compared_observables_agree')
    if not isinstance(agrees, bool) or agrees != (not union):
        raise ValueError('compared_observables_agree contradicts difference_frames')
    if report.get('full_timeline_equivalence') is not False:
        raise ValueError('full_timeline_equivalence must be false for this comparator')

    tail = report.get('matching_tail')
    if not isinstance(tail, dict):
        raise ValueError('matching_tail must be an object')
    tail_first = _integer(tail.get('first_original_frame'),
                          'matching_tail.first_original_frame')
    tail_frames = _integer(tail.get('frames'), 'matching_tail.frames')
    tail_controller = _integer(tail.get('controller_samples'),
                               'matching_tail.controller_samples')
    expected_tail_first = max(union + all_missing, default=-1) + 1
    if tail_first != expected_tail_first:
        raise ValueError('matching_tail.first_original_frame contradicts the last difference/missing frame')
    if tail_first > total or tail_frames != total - tail_first:
        raise ValueError('matching_tail.frames contradicts original_frames and first_original_frame')
    if tail_controller > controller_samples or tail_controller > tail_frames:
        raise ValueError('matching_tail.controller_samples exceeds available samples or tail frames')
    if controller_samples - tail_controller > tail_first:
        raise ValueError('matching_tail.controller_samples leaves too few pre-tail frames for controller samples')

    artifacts = report.get('sha256')
    if not isinstance(artifacts, dict) or not all(
            isinstance(name, str) and isinstance(digest, str)
            for name, digest in artifacts.items()):
        raise ValueError('sha256 must preserve the source artifact hash object')
    for key in ('scope', 'elf_sha256', 'native_run', 'original_run',
                'position_tolerance'):
        if key not in report:
            raise ValueError(f'missing required provenance/comparison field: {key}')

    first_missing = all_missing[0] if all_missing else None
    first_missing_kinds = [f'missing_original_{kind}_observation'
                           for kind, frames in missing_by_kind.items() if first_missing in frames]
    return {
        'schema_version': 1,
        'source_report': {
            'path': str(source_path),
            'sha256': source_sha256,
            'artifact_sha256': artifacts,
            'artifact_sha256_status': 'retained provenance; hashes were not revalidated against files',
        },
        'comparison': {
            'scope': report['scope'],
            'elf_sha256': report['elf_sha256'],
            'native_run': report['native_run'],
            'original_run': report['original_run'],
            'first_native_frame': anchor,
            'original_frames': total,
            'controller_samples': controller_samples,
            'position_tolerance': report['position_tolerance'],
            'compared_observables_agree': agrees,
            'complete_original_observation_coverage': complete_coverage,
            'original_observation_coverage': coverage,
            'full_timeline_equivalence': False,
            **{key: report[key] for key in (
                'no_dialogue_interval', 'unblended_target_check',
                'unblended_target_compared_frames', 'hero_position_maximum_error',
                'camera_position_maximum_error') if key in report},
        },
        'frame_mapping': {
            'native_tick_formula': 'first_native_frame + original_frame',
            'note': ('This reports the native trace tick label associated with each original frame. '
                     'The source comparator keeps its established before/after update pairing, '
                     'including preceding completed native-frame observations; no frames are shifted.'),
        },
        'divergence': {
            'first_difference': None if first_difference_frame is None else {
                'original_frame': first_difference_frame,
                'native_simulation_tick': anchor + first_difference_frame,
                'categories': first_categories,
            },
            'category_frame_counts': {
                name: len(frames) for name, frames in category_frames.items()
            },
            'unique_differing_frames': len(union),
            'first_missing_observation': None if first_missing is None else {
                'original_frame': first_missing,
                'native_simulation_tick': anchor + first_missing,
                'kind': first_missing_kinds[0],
            },
            'first_missing_observation_kinds': first_missing_kinds,
            'missing_original_manager_frames': len(missing_manager),
            'missing_manager_ranges': _ranges(missing_manager, anchor),
            'missing_original_camera_frames': len(missing_camera),
            'missing_camera_ranges': _ranges(missing_camera, anchor),
            'unique_missing_observation_frames': len(all_missing),
            'missing_original_player_frames': len(missing),
            'missing_player_ranges': _ranges(missing, anchor),
            'matching_tail': {
                'first_original_frame': tail_first,
                'first_native_tick': anchor + tail_first,
                'frames': tail_frames,
                'controller_samples': tail_controller,
            },
        },
        'limits': {
            'per_frame_image_comparison': False,
            'aligned_audio_comparison': False,
            'whole_timeline_equivalence_claim': False,
            'difference_frame_lists_count_frames_not_words': True,
            'agreement_applies_only_to_compared_observables': True,
            'coverage_scope': 'Original manager, camera and player observations only; controller samples are conditional.',
            'missing_player_list_scope': 'Present manager records without a player payload; player coverage also includes absent managers.',
        },
    }

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True,
                        help='Existing diagnose_home1b_interval.py JSON report')
    parser.add_argument('--output', type=Path, required=True,
                        help='New summary JSON path; existing files are refused')
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f'refusing existing output: {args.output}')
    try:
        raw = args.input.read_bytes()
        source = json.loads(raw)
        result = summarize(source, source_path=args.input,
                           source_sha256=hashlib.sha256(raw).hexdigest())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('x', encoding='utf-8') as output:
            output.write(json.dumps(result, indent=2) + '\n')
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(result['divergence'], sort_keys=True))

if __name__ == '__main__':
    main()
