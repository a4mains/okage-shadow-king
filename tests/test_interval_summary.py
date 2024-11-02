import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'interval_summary', Path(__file__).resolve().parents[1] / 'tools/summarize_interval_report.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def sample_report():
    return {
        'scope': "Synthetic fixed-anchor interval.",
        'elf_sha256': 'elf-hash',
        'native_run': {'exit_code': 0, 'binary_sha256': 'native-binary'},
        'original_run': {'exit_code': 0, 'binary_sha256': 'oracle-binary'},
        'first_native_frame': 100,
        'original_frames': 20,
        'controller_samples': 18,
        'difference_frames': {
            'controller': [3, 4, 9],
            'camera_position': [4, 10],
            'hero_position': [],
        },
        'missing_original_player_frames': [1, 2, 8],
        'compared_observables_agree': False,
        'full_timeline_equivalence': False,
        'no_dialogue_interval': False,
        'unblended_target_check': True,
        'unblended_target_compared_frames': [],
        'hero_position_maximum_error': 0.0,
        'camera_position_maximum_error': 0.00001,
        'position_tolerance': 0.00002,
        'matching_tail': {
            'first_original_frame': 11,
            'frames': 9,
            'controller_samples': 8,
        },
        'sha256': {'native.trace': 'trace-hash', 'reference.trace': 'reference-hash'},
    }

def sparse_native_report():
    source = sample_report()
    source['sparse_native'] = True
    source['native_frame_mapping'] = dict(
        current='first_frame + original_frame',
        prior='first_frame + original_frame - 1')
    channels = ('manager', 'movement', 'dialogue', 'choice', 'camera',
                'camera-state', 'actor')
    source['missing_native_frames_by_channel'] = {name: [19] for name in channels}
    source['native_observation_coverage'] = {
        name: dict(expected_frames=20, observed_frames=19, missing_frames=1)
        for name in channels}
    observables = ('controller', 'choice', 'controller_manager_pad_gate',
                   'manager_running_pc_gate', 'pad_latch', 'direction_latch',
                   'player_mask', 'hero_facing_w', 'hero_position',
                   'camera_rotation', 'camera_mode_phase', 'camera_position',
                   'camera_unblended_target')
    dependencies = dict(
        controller='dialogue', choice='choice', controller_manager_pad_gate='manager',
        manager_running_pc_gate='manager', pad_latch='manager', direction_latch='movement',
        player_mask='manager', hero_facing_w='actor', hero_position='actor',
        camera_rotation='camera', camera_mode_phase='camera-state', camera_position='camera',
        camera_unblended_target='camera')
    source['missing_native_observable_frames'] = {name: [19] for name in observables}
    source['compared_observable_frames'] = {name: list(range(19)) for name in observables}
    source['missing_native_dependencies'] = {
        name: [dict(original_frame=19, dependencies=[
            dict(channel=dependencies[name], native_frame=119)])]
        for name in observables}
    source['observable_coverage'] = {
        name: dict(expected_observations=20, compared_observations=19,
                   missing_native_observations=1)
        for name in observables}
    source['complete_native_observation_coverage'] = False
    source['complete_observation_coverage'] = False
    source['matching_tail'] = dict(first_original_frame=20, frames=0, controller_samples=0)
    return source

class IntervalSummaryTests(unittest.TestCase):
    def test_summarizes_first_difference_missing_ranges_and_provenance(self):
        source = sample_report()
        result = module.summarize(source, source_path='report.json', source_sha256='report-hash')

        self.assertEqual(result['source_report']['sha256'], 'report-hash')
        self.assertEqual(result['source_report']['artifact_sha256'], source['sha256'])
        self.assertEqual(result['comparison']['native_run'], source['native_run'])
        self.assertEqual(result['comparison']['original_run'], source['original_run'])
        self.assertEqual(result['comparison']['scope'], source['scope'])
        self.assertEqual(result['comparison']['position_tolerance'], 0.00002)
        self.assertFalse(result['comparison']['no_dialogue_interval'])
        self.assertTrue(result['comparison']['unblended_target_check'])
        self.assertEqual(result['comparison']['unblended_target_compared_frames'], [])
        self.assertEqual(result['comparison']['hero_position_maximum_error'], 0.0)
        self.assertEqual(result['comparison']['camera_position_maximum_error'], 0.00001)
        self.assertIn('not revalidated against files',
                      result['source_report']['artifact_sha256_status'])
        self.assertEqual(result['divergence']['first_difference'], {
            'original_frame': 3,
            'native_simulation_tick': 103,
            'categories': ['controller'],
        })
        self.assertEqual(result['divergence']['category_frame_counts'], {
            'controller': 3, 'camera_position': 2, 'hero_position': 0,
        })
        self.assertEqual(result['divergence']['unique_differing_frames'], 4)
        self.assertEqual(result['divergence']['first_missing_observation'], {
            'original_frame': 1,
            'native_simulation_tick': 101,
            'kind': 'missing_original_player_observation',
        })
        self.assertEqual(result['divergence']['missing_player_ranges'], [
            {'original_start_frame': 1, 'original_end_frame': 2,
             'native_tick_start': 101, 'native_tick_end': 102, 'frames': 2},
            {'original_start_frame': 8, 'original_end_frame': 8,
             'native_tick_start': 108, 'native_tick_end': 108, 'frames': 1},
        ])
        self.assertEqual(result['divergence']['matching_tail'], {
            'first_original_frame': 11, 'first_native_tick': 111,
            'frames': 9, 'controller_samples': 8,
        })
        self.assertFalse(result['limits']['per_frame_image_comparison'])
        self.assertFalse(result['limits']['aligned_audio_comparison'])
        self.assertFalse(result['limits']['whole_timeline_equivalence_claim'])

    def test_empty_differences_and_missing_observations_keep_tail(self):
        source = sample_report()
        source['difference_frames'] = {'controller': [], 'camera_position': []}
        source['missing_original_player_frames'] = []
        source['compared_observables_agree'] = True
        source['matching_tail'] = {
            'first_original_frame': 0, 'frames': 20, 'controller_samples': 18,
        }
        result = module.summarize(source)
        self.assertIsNone(result['divergence']['first_difference'])
        self.assertIsNone(result['divergence']['first_missing_observation'])
        self.assertEqual(result['divergence']['unique_differing_frames'], 0)
        self.assertEqual(result['divergence']['matching_tail']['frames'], 20)

    def test_rejects_unsorted_duplicate_and_out_of_range_frames(self):
        for frames in ([4, 3], [3, 3], [20]):
            with self.subTest(frames=frames):
                source = sample_report()
                source['difference_frames']['controller'] = frames
                source['matching_tail'] = {
                    'first_original_frame': 11, 'frames': 9, 'controller_samples': 8,
                }
                with self.assertRaises(ValueError):
                    module.summarize(source)

    def test_rejects_contradictory_agreement_and_tail_fields(self):
        source = sample_report()
        source['compared_observables_agree'] = True
        with self.assertRaisesRegex(ValueError, 'compared_observables_agree'):
            module.summarize(source)

        source = sample_report()
        source['matching_tail']['first_original_frame'] = 10
        with self.assertRaisesRegex(ValueError, 'last difference/missing'):
            module.summarize(source)

        source = sample_report()
        source['matching_tail']['frames'] = 8
        with self.assertRaisesRegex(ValueError, 'matching_tail.frames'):
            module.summarize(source)

        source = sample_report()
        source['matching_tail']['controller_samples'] = 10
        with self.assertRaisesRegex(ValueError, 'controller_samples exceeds'):
            module.summarize(source)

    def test_rejects_invalid_anchor_and_impossible_controller_accounting(self):
        source = sample_report()
        source['first_native_frame'] = 1
        with self.assertRaisesRegex(ValueError, 'first_native_frame'):
            module.summarize(source)

        source = sample_report()
        source['controller_samples'] = 2
        with self.assertRaisesRegex(ValueError, 'difference_frames.controller'):
            module.summarize(source)

        source = sample_report()
        source['matching_tail']['controller_samples'] = 6
        with self.assertRaisesRegex(ValueError, 'too few pre-tail frames'):
            module.summarize(source)

    def test_sparse_native_missing_tail_and_original_counts_are_preserved(self):
        result = module.summarize(sparse_native_report())
        self.assertEqual(result['divergence']['matching_tail'], {
            'first_original_frame': 20, 'first_native_tick': 120,
            'frames': 0, 'controller_samples': 0,
        })
        self.assertEqual(result['divergence']['unique_missing_observation_frames'], 3)
        self.assertEqual(sum(row['frames'] for row in result['divergence']['missing_player_ranges']), 3)
        self.assertEqual(result['divergence']['missing_native_channel_counts'], {
            name: 1 for name in ('manager', 'movement', 'dialogue', 'choice',
                                 'camera', 'camera-state', 'actor')})
        self.assertEqual(result['divergence']['missing_native_observable_counts']['hero_position'], 1)
        self.assertFalse(result['comparison']['complete_native_observation_coverage'])

    def test_sparse_native_allows_first_frame_prior_dependency(self):
        source = sparse_native_report()
        name = 'camera_unblended_target'
        source['missing_native_observable_frames'][name] = [0, 19]
        source['compared_observable_frames'][name] = list(range(1, 19))
        source['missing_native_dependencies'][name] = [
            dict(original_frame=0, dependencies=[dict(channel='camera', native_frame=99)]),
            dict(original_frame=19, dependencies=[dict(channel='camera', native_frame=119)]),
        ]
        source['observable_coverage'][name] = dict(
            expected_observations=20, compared_observations=18,
            missing_native_observations=2)

        result = module.summarize(source)
        self.assertEqual(result['divergence']['matching_tail']['first_original_frame'], 20)
        self.assertEqual(result['divergence']['missing_native_observable_counts'][name], 2)

    def test_sparse_native_corrupted_tail_and_dependencies_are_rejected(self):
        source = sparse_native_report()
        source['matching_tail'] = dict(first_original_frame=19, frames=1, controller_samples=0)
        with self.assertRaisesRegex(ValueError, 'last difference/missing'):
            module.summarize(source)

        source = sparse_native_report()
        source['missing_native_dependencies']['hero_position'][0]['dependencies'][0]['native_frame'] = 118
        with self.assertRaisesRegex(ValueError, 'does not point to a missing native channel frame'):
            module.summarize(source)

        source = sparse_native_report()
        source['missing_native_frames_by_channel']['actor'] = [17, 19]
        source['native_observation_coverage']['actor'] = dict(
            expected_frames=20, observed_frames=18, missing_frames=2)
        source['missing_native_dependencies']['hero_position'][0]['dependencies'][0]['native_frame'] = 117
        with self.assertRaisesRegex(ValueError, 'not the current or prior native frame'):
            module.summarize(source)

    def test_cli_refuses_existing_and_dangling_symlink_outputs(self):
        tool = Path(__file__).resolve().parents[1] / 'tools/summarize_interval_report.py'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'source.json'
            source.write_text(json.dumps(sample_report()))
            existing = root / 'existing.json'
            existing.write_text('preserve me')
            result = subprocess.run(
                [sys.executable, str(tool), '--input', str(source), '--output', str(existing)],
                capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(existing.read_text(), 'preserve me')

            dangling = root / 'dangling.json'
            dangling.symlink_to(root / 'missing-target.json')
            result = subprocess.run(
                [sys.executable, str(tool), '--input', str(source), '--output', str(dangling)],
                capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(dangling.is_symlink())

    def test_rejects_malformed_missing_list_and_word_count_invention(self):
        source = sample_report()
        source['missing_original_player_frames'] = [2, 1]
        with self.assertRaisesRegex(ValueError, 'strictly increasing'):
            module.summarize(source)
        result = module.summarize(sample_report())
        self.assertNotIn('difference_words', result['divergence'])

if __name__ == '__main__':
    unittest.main()
