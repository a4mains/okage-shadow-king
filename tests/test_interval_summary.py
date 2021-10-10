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
