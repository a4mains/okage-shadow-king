import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from diagnose_home1b_interval import compare_observations
from summarize_interval_report import summarize

def word(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]

def record(width, frame, values=None):
    result = [0] * width
    result[0] = frame
    for offset, value in (values or {}).items():
        result[offset] = value
    return tuple(result)

def fixture(total=6):
    return dict(
        total=total, first_frame=2,
        managers=[record(8, tick) for tick in range(1, total+2)],
        movement=[record(16, tick) for tick in range(1, total+2)],
        dialogue=[record(12, tick) for tick in range(1, total+2)],
        choices=[record(12, tick) for tick in range(1, total+2)],
        cameras=[record(16, tick, {4: word(tick-2)}) for tick in range(1, total+2)],
        camera_states=[record(5, tick, {1: 2, 3: word(1)}) for tick in range(1, total+2)],
        actors=[record(16, tick, {12: word(tick-1)}) for tick in range(1, total+2)],
        transitions=[record(128, f, {1: 0x133a98, 28: 1, 44: word(f)}) for f in range(total)],
        camera=[record(32, f, {2: 2, 3: word(1), 4: word(f)}) for f in range(total)],
        controller=[], original_choices=[], unblended_target=True)

def report_for(arguments):
    return dict(scope='Synthetic fixed anchor.', elf_sha256='synthetic', native_run={},
                original_run={}, first_native_frame=arguments['first_frame'],
                original_frames=arguments['total'], sha256={}, **compare_observations(**arguments))

class IntervalObservationTests(unittest.TestCase):
    def test_complete_observations_and_stable_target(self):
        result = compare_observations(**fixture())
        self.assertTrue(result['compared_observables_agree'])
        self.assertTrue(result['complete_original_observation_coverage'])
        self.assertEqual(result['unblended_target_compared_frames'], [1, 2, 3, 4, 5])
        self.assertEqual(result['matching_tail']['frames'], 6)

    def test_shared_gap_is_incomplete_even_when_observed_values_agree(self):
        args = fixture()
        for name in ('transitions', 'camera'):
            args[name] = [r for r in args[name] if r[0] not in (2, 3)]
        result = report_for(args)
        self.assertTrue(result['compared_observables_agree'])
        self.assertFalse(result['complete_original_observation_coverage'])
        self.assertEqual(result['missing_original_manager_frames'], [2, 3])
        self.assertEqual(result['missing_original_camera_frames'], [2, 3])
        self.assertEqual(result['missing_original_player_frames'], [])
        self.assertEqual(result['original_observation_coverage']['player']['observed_frames'], 4)
        self.assertEqual(result['matching_tail']['first_original_frame'], 4)
        self.assertEqual(result['unblended_target_compared_frames'], [1, 5])
        summary = summarize(result)
        self.assertFalse(summary['comparison']['complete_original_observation_coverage'])
        self.assertEqual(summary['divergence']['first_missing_observation_kinds'],
                         ['missing_original_manager_observation', 'missing_original_camera_observation'])
        self.assertEqual(summary['divergence']['missing_manager_ranges'][0]['frames'], 2)
        self.assertEqual(summary['divergence']['unique_missing_observation_frames'], 2)

    def test_channels_compare_independently_at_fixed_labels(self):
        args = fixture()
        args['transitions'] = [r for r in args['transitions'] if r[0] != 2]
        args['camera'] = [r for r in args['camera'] if r[0] != 3]
        args['cameras'][3] = record(16, 4, {4: word(9)})  # original frame 2
        args['movement'][4] = record(16, 5, {1: 0x1000})  # original frame 3
        result = compare_observations(**args)
        self.assertEqual(result['difference_frames']['camera_position'], [2])
        self.assertEqual(result['difference_frames']['direction_latch'], [3])
        self.assertEqual(result['difference_frames']['hero_position'], [])
        self.assertEqual(result['missing_original_manager_frames'], [2])
        self.assertEqual(result['missing_original_camera_frames'], [3])

    def test_target_requires_actual_previous_frame_and_current_manager(self):
        args = fixture()
        args['camera'] = [r for r in args['camera'] if r[0] != 2]
        args['transitions'] = [r for r in args['transitions'] if r[0] != 4]
        for f in (3, 4, 5):
            args['cameras'][f+1] = record(16, f+2, {4: word(f), 8: word(9)})
        result = compare_observations(**args)
        self.assertEqual(result['unblended_target_compared_frames'], [1, 5])
        self.assertEqual(result['difference_frames']['camera_unblended_target'], [5])

    def test_rejects_duplicate_reordered_out_of_range_labels(self):
        for name in ('transitions', 'camera'):
            for labels in ([0, 1, 1, 3], [1, 0, 2], [-1, 0], [0, 6]):
                with self.subTest(channel=name, labels=labels):
                    args = fixture()
                    template = args[name][0]
                    args[name] = [(f,) + template[1:] for f in labels]
                    with self.assertRaisesRegex(ValueError, 'outside|strictly increasing'):
                        compare_observations(**args)

    def test_native_channel_labels_are_validated(self):
        args = fixture()
        args['cameras'][2] = args['cameras'][1]
        with self.assertRaisesRegex(ValueError, 'Native camera frame sequence'):
            compare_observations(**args)

    def test_absent_channels_and_player_payloads_are_explicit(self):
        args = fixture()
        args['camera'] = []
        args['transitions'][4] = record(128, 4)
        result = report_for(args)
        self.assertTrue(result['compared_observables_agree'])
        self.assertEqual(result['missing_original_camera_frames'], list(range(6)))
        self.assertEqual(result['missing_original_player_frames'], [4])
        self.assertEqual(result['matching_tail']['frames'], 0)
        self.assertEqual(summarize(result)['comparison']['original_observation_coverage']['camera'],
                         dict(expected_frames=6, observed_frames=0))

    def test_summary_rejects_inconsistent_coverage_targets_and_tail(self):
        for change in ('complete', 'count', 'tail', 'target', 'difference'):
            with self.subTest(change=change):
                args = fixture()
                args['camera'] = [r for r in args['camera'] if r[0] != 2]
                result = report_for(args)
                if change == 'complete':
                    result['complete_original_observation_coverage'] = True
                elif change == 'count':
                    result['original_observation_coverage']['camera']['observed_frames'] = 6
                elif change == 'tail':
                    result['matching_tail'] = dict(first_original_frame=0, frames=6, controller_samples=0)
                elif change == 'target':
                    result['unblended_target_compared_frames'] = [1, 3, 4, 5]
                else:
                    result['difference_frames']['camera_position'] = [2]
                    result['compared_observables_agree'] = False
                with self.assertRaises(ValueError):
                    summarize(result)

    def test_summary_rejects_malformed_new_missing_lists(self):
        for name in ('manager', 'camera'):
            for missing in ([2, 2], [3, 2], [6], [-1]):
                with self.subTest(channel=name, missing=missing):
                    result = report_for(fixture())
                    result[f'missing_original_{name}_frames'] = missing
                    with self.assertRaises(ValueError):
                        summarize(result)

NATIVE_CHANNELS = ('managers', 'movement', 'dialogue', 'choices', 'cameras', 'camera_states', 'actors')

class SparseNativeObservationTests(unittest.TestCase):
    def test_complete_strict_results_unchanged(self):
        args = fixture()
        strict = compare_observations(**args)
        sparse = compare_observations(**args, sparse_native=True)
        self.assertEqual(strict, {key: sparse[key] for key in strict})
        self.assertTrue(sparse['complete_native_observation_coverage'])
        self.assertFalse(sparse['full_timeline_equivalence'])
        self.assertEqual(sparse['observable_coverage']['hero_position'], dict(
            expected_observations=6, compared_observations=6, missing_native_observations=0))

    def test_middle_battle_gap_uses_labels_and_exact_prior(self):
        args = fixture()
        for name in NATIVE_CHANNELS:
            args[name] = [r for r in args[name] if r[0] not in (3, 4)]
        with self.assertRaisesRegex(ValueError, 'frame sequence'):
            compare_observations(**args)
        result = compare_observations(**args, sparse_native=True)
        self.assertTrue(result['compared_observables_agree'])
        self.assertFalse(result['complete_native_observation_coverage'])
        self.assertFalse(result['complete_observation_coverage'])
        self.assertEqual(result['missing_native_frames_by_channel']['actor'], [1, 2])
        self.assertEqual(result['missing_native_observable_frames']['hero_position'], [2, 3])
        self.assertEqual(result['compared_observable_frames']['hero_position'], [0, 1, 4, 5])
        self.assertEqual(result['compared_observable_frames']['camera_position'], [0, 3, 4, 5])
        self.assertEqual(result['matching_tail'], dict(first_original_frame=4, frames=2, controller_samples=0))
        self.assertEqual(result['missing_native_dependencies']['hero_position'], [
            dict(original_frame=2, dependencies=[dict(channel='actor', native_frame=3)]),
            dict(original_frame=3, dependencies=[dict(channel='actor', native_frame=4)])])

    def test_channel_specific_missing_current_and_prior(self):
        args = fixture()
        args['actors'] = [r for r in args['actors'] if r[0] != 1]
        args['camera_states'] = [r for r in args['camera_states'] if r[0] != 4]
        args['cameras'][3] = record(16, 4, {4: word(99)})
        result = compare_observations(**args, sparse_native=True)
        self.assertEqual(result['difference_frames']['camera_position'], [2])
        self.assertEqual(result['missing_native_frames_by_channel']['actor'], [])
        self.assertEqual(result['missing_native_observable_frames']['hero_position'], [0])
        self.assertEqual(result['missing_native_observable_frames']['camera_mode_phase'], [2])
        self.assertEqual(result['missing_native_observable_frames']['camera_unblended_target'], [2, 3])
        self.assertEqual(result['unblended_target_compared_frames'], [1, 4, 5])
        self.assertEqual(result['observable_coverage']['hero_position']['compared_observations'], 5)
        self.assertFalse(result['complete_native_observation_coverage'])

    def test_late_returned_field_has_no_synthetic_prefix_or_reanchoring(self):
        args = fixture()
        offset = 109774
        args['first_frame'] += offset
        for name in NATIVE_CHANNELS:
            args[name] = [(r[0]+offset,) + r[1:] for r in args[name] if r[0] >= 5]
        result = compare_observations(**args, sparse_native=True)
        self.assertEqual(result['compared_observable_frames']['camera_position'], [3, 4, 5])
        self.assertEqual(result['compared_observable_frames']['hero_position'], [4, 5])
        self.assertTrue(result['compared_observables_agree'])
        self.assertEqual(result['native_observation_coverage']['camera'], dict(
            expected_frames=6, observed_frames=3, missing_frames=3))
        self.assertEqual(result['missing_native_dependencies']['hero_position'][0],
                         dict(original_frame=0, dependencies=[dict(channel='actor', native_frame=109775)]))
        self.assertEqual(result['matching_tail']['frames'], 2)

    def test_native_labels_reject_duplicates_reordering_and_invalid_types(self):
        for name in NATIVE_CHANNELS:
            for labels in ([1, 2, 2], [2, 1], [0, 1], [-1, 1], [True, 2], [1.5, 2]):
                with self.subTest(channel=name, labels=labels):
                    args = fixture()
                    args[name] = [(f,) + args[name][0][1:] for f in labels]
                    with self.assertRaisesRegex(ValueError, 'positive and strictly increasing'):
                        compare_observations(**args, sparse_native=True)

    def test_empty_native_never_produces_matching_tail(self):
        args = fixture()
        for name in NATIVE_CHANNELS:
            args[name] = []
        result = compare_observations(**args, sparse_native=True)
        self.assertEqual(result['matching_tail']['frames'], 0)
        self.assertFalse(result['complete_observation_coverage'])
        self.assertTrue(all(not f for f in result['compared_observable_frames'].values()))
        self.assertEqual(result['missing_native_observable_frames']['hero_position'], list(range(6)))

    def test_missing_original_manager_and_player_still_cover_full_interval(self):
        args = fixture(total=600)
        args['transitions'] = []
        for name in NATIVE_CHANNELS:
            args[name] = [r for r in args[name] if r[0] >= 500]
        result = compare_observations(**args, sparse_native=True)
        self.assertEqual(result['missing_original_manager_frames'], list(range(600)))
        self.assertEqual(result['original_observation_coverage']['player']['observed_frames'], 0)
        self.assertEqual(result['missing_original_player_frames'], [])  # historical payload-only meaning
        self.assertEqual(result['missing_native_observable_frames']['hero_position'], [])
        self.assertEqual(result['compared_observable_frames']['hero_position'], [])
        self.assertEqual(result['matching_tail']['frames'], 0)
        self.assertEqual(result['native_observation_coverage']['camera']['observed_frames'], 102)

    def test_controller_dependencies_are_independent(self):
        args = fixture()
        args['controller'] = [record(64, 1, {1: 0x148dc8}), record(64, 1, {1: 0x11ff38})]
        args['original_choices'] = [record(16, 1, {1: 0x148dc8}), record(16, 1, {1: 0x11ff38})]
        args['dialogue'] = [r for r in args['dialogue'] if r[0] != 3]
        args['managers'] = [r for r in args['managers'] if r[0] != 3]
        args['choices'][2] = record(12, 3, {2: 9})
        result = compare_observations(**args, sparse_native=True)
        self.assertEqual(result['controller_samples'], 1)  # observed source samples
        self.assertEqual(result['compared_observable_frames']['controller'], [])
        self.assertEqual(result['missing_native_observable_frames']['controller'], [1])
        self.assertEqual(result['missing_native_observable_frames']['controller_manager_pad_gate'], [1])
        self.assertEqual(result['difference_frames']['choice'], [1])
        self.assertEqual(result['compared_observable_frames']['choice'], [1])

    def test_controller_manager_pad_gate_mismatch_is_retained_as_difference(self):
        args = fixture()
        args['controller'] = [record(64, 1, {1: 0x148dc8}), record(64, 1, {1: 0x11ff38})]
        args['original_choices'] = [record(16, 1, {1: 0x148dc8}), record(16, 1, {1: 0x11ff38})]
        args['managers'][2] = record(8, 3, {1: 42})
        result = compare_observations(**args, sparse_native=True)
        self.assertEqual(result['difference_frames']['controller_manager_pad_gate'], [1])
        self.assertEqual(result['compared_observable_frames']['controller_manager_pad_gate'], [1])
        self.assertEqual(result['missing_native_observable_frames']['controller_manager_pad_gate'], [])
        self.assertEqual(result['observable_coverage']['controller_manager_pad_gate'], dict(
            expected_observations=1, compared_observations=1, missing_native_observations=0))
        self.assertFalse(result['compared_observables_agree'])

    def test_controller_manager_pad_gate_matches_and_missing_samples_are_counted(self):
        args = fixture()
        args['controller'] = []
        args['original_choices'] = []
        for frame, pad in ((1, 7), (2, 9)):
            before = record(64, frame, {1: 0x148dc8})
            after = record(64, frame, {1: 0x11ff38, 9: pad})
            args['controller'].extend((before, after))
            args['original_choices'].extend((
                record(16, frame, {1: 0x148dc8}),
                record(16, frame, {1: 0x11ff38})))
            args['managers'][frame + 1] = record(8, frame + 2, {1: pad})
            args['transitions'][frame] = record(128, frame, {
                1: 0x133a98, 28: 1, 33: pad << 16, 44: word(frame)})
        args['managers'] = [r for r in args['managers'] if r[0] != 4]

        result = compare_observations(**args, sparse_native=True)
        self.assertEqual(result['controller_samples'], 2)
        self.assertEqual(result['difference_frames']['controller_manager_pad_gate'], [])
        self.assertEqual(result['compared_observable_frames']['controller_manager_pad_gate'], [1])
        self.assertEqual(result['missing_native_observable_frames']['controller_manager_pad_gate'], [2])
        self.assertEqual(result['missing_native_dependencies']['controller_manager_pad_gate'], [
            dict(original_frame=2, dependencies=[dict(channel='manager', native_frame=4)])])
        self.assertEqual(result['observable_coverage']['controller_manager_pad_gate'], dict(
            expected_observations=2, compared_observations=1, missing_native_observations=1))
        self.assertTrue(result['compared_observables_agree'])

if __name__ == '__main__':
    unittest.main()
