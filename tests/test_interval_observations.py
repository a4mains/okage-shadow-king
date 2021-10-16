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

if __name__ == '__main__':
    unittest.main()
