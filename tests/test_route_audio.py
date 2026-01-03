import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from compare_route_audio import compare_route_audio, extract_route_audio_boundaries

def write_wav(path, samples, rate):
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(b''.join(struct.pack('<hh', *pair) for pair in samples))

class CompareRouteAudioTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.original = self.root / 'original.wav'
        self.native = self.root / 'native.png.audio.pcm'
        self.capture_prefix = self.root / 'native'
        self.output_prefix = self.root / 'pair'
        # Two nominal 60 Hz ticks at 600 Hz: ten stereo sample frames per tick.
        self.original_samples = [(-3, 0), (4, 5)] * 10
        write_wav(self.original, self.original_samples, 600)

        # Tick 3 begins at sample-frame index 40. Extract ticks 3 and 4.
        self.native_samples = [(99, -99)] * 40 + [(1, -2), (0, 0)] * 20 + [(77, 88)] * 20
        self.native.write_bytes(b''.join(struct.pack('<hh', *pair) for pair in self.native_samples))
        self.audio_manifest = self.root / 'native.png.audio.json'
        self.audio_manifest.write_text(json.dumps({'dry_output': True, 'frames': 100, 'rate': 1200}))
        self.run_manifest = self.root / 'native.run.json'
        self.run_manifest.write_text(json.dumps({'exit_code': 0, 'command': ['native', '--frames', '5']}))

    def tearDown(self):
        self.temp.cleanup()

    def run_pair(self, *, rate=1200, first_tick=3, ticks=2):
        return compare_route_audio(self.original, self.native, rate, first_tick, ticks,
                                   self.output_prefix)

    def test_extracts_fixed_tick_segment_and_preserves_each_rate(self):
        report = self.run_pair()
        original_out = self.root / 'pair.original.wav'
        native_out = self.root / 'pair.native.wav'

        with wave.open(str(original_out), 'rb') as extracted:
            self.assertEqual((extracted.getnchannels(), extracted.getsampwidth(),
                              extracted.getframerate(), extracted.getnframes()),
                             (2, 2, 600, 20))
            self.assertEqual(extracted.readframes(20),
                             b''.join(struct.pack('<hh', *pair) for pair in self.original_samples))
        with wave.open(str(native_out), 'rb') as extracted:
            self.assertEqual((extracted.getnchannels(), extracted.getsampwidth(),
                              extracted.getframerate(), extracted.getnframes()),
                             (2, 2, 1200, 40))
            self.assertEqual(extracted.readframes(40),
                             b''.join(struct.pack('<hh', *pair) for pair in self.native_samples[40:80]))

        self.assertEqual(report['alignment']['native_start_sample_frame_zero_based'], 40)
        self.assertEqual(report['alignment']['native_end_sample_frame_exclusive'], 80)
        self.assertFalse(report['alignment']['sample_to_vblank_alignment_proven'])
        self.assertFalse(report['alignment']['waveform_alignment_performed'])
        self.assertFalse(report['alignment']['resampling_performed'])
        self.assertFalse(report['alignment']['waveform_equivalence_asserted'])
        self.assertEqual(report['segments']['original']['statistics']['left']['peak_abs'], 4)
        self.assertEqual(report['segments']['original']['statistics']['right']['nonzero_samples'], 10)
        self.assertAlmostEqual(report['segments']['original']['statistics']['left']['rms'],
                               math.sqrt(12.5))
        self.assertEqual(report['segments']['native']['statistics']['left']['peak_abs'], 1)
        self.assertEqual(report['segments']['native']['statistics']['right']['peak_abs'], 2)
        self.assertEqual(report['segments']['native']['statistics']['right']['nonzero_samples'], 20)
        self.assertEqual(report['sources']['manifests']['native_audio']['path'],
                         str(self.audio_manifest))
        self.assertEqual(report['sources']['manifests']['native_run']['run_ticks'], 5)
        self.assertTrue((self.root / 'pair.json').is_file())

    def test_rejects_original_wav_with_wrong_nominal_duration(self):
        write_wav(self.original, self.original_samples[:-1], 600)
        with self.assertRaisesRegex(ValueError, 'expected 20'):
            self.run_pair()

    def test_rejects_native_manifest_rate_and_extent_mismatches(self):
        self.audio_manifest.write_text(json.dumps({'frames': 100, 'rate': 48000}))
        with self.assertRaisesRegex(ValueError, 'disagrees.*rate'):
            self.run_pair()

        self.audio_manifest.write_text(json.dumps({'frames': 99, 'rate': 1200}))
        with self.assertRaisesRegex(ValueError, 'disagrees.*frames'):
            self.run_pair()

    def test_rejects_wrong_declared_native_format(self):
        self.audio_manifest.write_text(json.dumps({'frames': 100, 'rate': 1200, 'channels': 1}))
        with self.assertRaisesRegex(ValueError, 'channels must be 2'):
            self.run_pair()

    def test_validates_original_oracle_positional_vblank_count(self):
        original_run = self.root / 'original.run.json'
        original_run.write_text(json.dumps({
            'exit_code': 0,
            'command': ['oracle', 'disc.cue', '2', '--audio', str(self.original)],
        }))
        report = self.run_pair()
        self.assertEqual(report['sources']['manifests']['original_run']['run_ticks'], 2)

        original_run.write_text(json.dumps({
            'exit_code': 0,
            'command': ['oracle', 'disc.cue', '3', '--audio', str(self.original)],
        }))
        with self.assertRaisesRegex(ValueError, 'frame count disagrees'):
            compare_route_audio(self.original, self.native, 1200, 3, 2,
                                self.root / 'different')

    def test_rejects_native_tick_slice_outside_pcm_or_run(self):
        with self.assertRaisesRegex(ValueError, 'run frame count'):
            self.run_pair(first_tick=5, ticks=2)  # Source run manifest ends at tick 5.

        self.run_manifest.unlink()
        with self.assertRaisesRegex(ValueError, 'slice ends'):
            self.run_pair(first_tick=6, ticks=2)  # The raw PCM extent is too short.

    def test_rejects_original_manifest_naming_same_basename_in_another_directory(self):
        other = self.root / 'other'
        other.mkdir()
        wrong = other / self.original.name
        write_wav(wrong, self.original_samples, 600)
        (self.root / 'original.run.json').write_text(json.dumps({
            'exit_code': 0,
            'command': ['oracle', 'disc.cue', '2', '--audio', str(wrong)],
        }))
        with self.assertRaisesRegex(ValueError, 'does not name the input WAV'):
            self.run_pair()
        self.assertFalse((self.root / 'pair.original.wav').exists())

    def test_refuses_existing_or_dangling_symlink_outputs(self):
        collision = self.root / 'pair.original.wav'
        collision.write_bytes(b'keep')
        with self.assertRaisesRegex(ValueError, 'refusing existing output'):
            self.run_pair()
        self.assertEqual(collision.read_bytes(), b'keep')

        collision.unlink()
        symlink = self.root / 'pair.native.wav'
        symlink.symlink_to(self.root / 'missing-target')
        with self.assertRaisesRegex(ValueError, 'refusing existing output'):
            self.run_pair()

class BoundaryRouteAudioTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.original = self.root / 'original.wav'
        self.original_index = self.root / 'original.audio-index.json'
        self.original_run = self.root / 'original.run.json'
        self.native = self.root / 'native.png.audio.pcm'
        self.native_audio = self.root / 'native.png.audio.json'
        self.native_run = self.root / 'native.run.json'
        self.native_index = self.root / 'native.frames.json'
        self.output = self.root / 'boundary-pair'

        self.original_pairs = [(n, -n) for n in range(1, 31)]
        write_wav(self.original, self.original_pairs, 600)
        self.original_index.write_text(json.dumps({
            'sample_rate': 600,
            'channels': 2,
            'total_sample_frames': 30,
            'boundary': 'OnNewFrame after GS SetVBlank and PadHandler Update, before this iteration EE/IOP update',
            'scope': 'Offsets into emitted PCM; SPU output is block buffered. These are not audible-event times.',
            'vblanks': [
                {'vblank_start': 1, 'sample_frame_offset': 0},
                {'vblank_start': 2, 'sample_frame_offset': 10},
                {'vblank_start': 3, 'sample_frame_offset': 25},
            ],
        }))
        self.original_run.write_text(json.dumps({
            'exit_code': 0,
            'command': ['oracle', 'disc.cue', '3', '--audio', str(self.original)],
        }))

        self.native_pairs = [(100+n, -100-n) for n in range(100)]
        self.native.write_bytes(b''.join(struct.pack('<hh', *pair) for pair in self.native_pairs))
        self.native_audio.write_text(json.dumps({'frames': 100, 'rate': 1200}))
        self.native_run.write_text(json.dumps({'exit_code': 0,
                                              'command': ['native', '--frames', '5']}))
        self.native_index.write_text(json.dumps({
            'boundary': 'after completed native Session::tick, before the next input',
            'captures': [
                {'tick': 3, 'audio_sample_frame_offset': 20},
                {'tick': 5, 'audio_sample_frame_offset': 40},
            ],
        }))

    def tearDown(self):
        self.temp.cleanup()

    def boundary_report(self, *, start=1, end='stop', native_index=None,
                        native_start=3, native_end=5, output=None):
        return extract_route_audio_boundaries(
            self.original, self.original_index, start, end, self.native, 1200,
            output or self.output, native_frame_index=native_index or self.native_index,
            native_start_tick=native_start, native_end_tick=native_end)

    def test_uses_recorded_boundaries_and_preserves_duration_mismatch(self):
        report = self.boundary_report()
        original_bytes = b''.join(struct.pack('<hh', *pair) for pair in self.original_pairs)
        native_bytes = b''.join(struct.pack('<hh', *pair) for pair in self.native_pairs[20:40])
        with wave.open(str(self.root / 'boundary-pair.original.wav'), 'rb') as result:
            self.assertEqual((result.getframerate(), result.getnframes()), (600, 30))
            self.assertEqual(result.readframes(30), original_bytes)
        with wave.open(str(self.root / 'boundary-pair.native.wav'), 'rb') as result:
            self.assertEqual((result.getframerate(), result.getnframes()), (1200, 20))
            self.assertEqual(result.readframes(20), native_bytes)
        self.assertEqual(report['status'], 'captured_unpaired_duration_mismatch')
        self.assertEqual(report['original_boundary']['end_boundary'],
                         'post_stop_total_sample_frames')
        self.assertTrue(report['original_boundary']['post_stop_endpoint_used'])
        self.assertEqual(report['native_boundary']['scope'], 'recorded_native_capture_boundaries')
        self.assertEqual(report['alignment']['segment_frame_count_difference'], -10)
        self.assertEqual(report['alignment']['segment_frame_count_difference_unit'],
                         'sample frames; not a duration')
        self.assertEqual(report['alignment']['duration_difference_rational_seconds'],
                         {'numerator': -1, 'denominator': 30})
        self.assertFalse(report['alignment']['temporal_alignment_proven'])
        self.assertFalse(report['alignment']['waveform_equivalence_asserted'])
        self.assertNotIn('comparison', report)

    def test_equal_duration_with_different_rates_is_not_a_mismatch(self):
        index = self.root / 'equal-duration.frames.json'
        index.write_text(json.dumps({
            'boundary': 'after completed native Session::tick, before the next input',
            'captures': [
                {'tick': 3, 'audio_sample_frame_offset': 20},
                {'tick': 5, 'audio_sample_frame_offset': 80},
            ],
        }))
        report = self.boundary_report(native_index=index, output=self.root/'equal-duration')
        self.assertEqual(report['status'], 'captured_segments_unaligned')
        self.assertFalse(report['alignment']['duration_mismatch'])
        self.assertEqual(report['alignment']['original_duration_seconds'], 0.05)
        self.assertEqual(report['alignment']['native_duration_seconds'], 0.05)
        self.assertEqual(report['alignment']['duration_difference_rational_seconds'],
                         {'numerator': 0, 'denominator': 1})
        self.assertEqual(report['alignment']['segment_frame_count_difference'], 30)

    def test_equal_frame_counts_with_different_rates_are_a_duration_mismatch(self):
        index = self.root / 'equal-count.frames.json'
        index.write_text(json.dumps({
            'boundary': 'after completed native Session::tick, before the next input',
            'captures': [
                {'tick': 3, 'audio_sample_frame_offset': 20},
                {'tick': 5, 'audio_sample_frame_offset': 50},
            ],
        }))
        report = self.boundary_report(native_index=index, output=self.root/'equal-count')
        self.assertEqual(report['status'], 'captured_unpaired_duration_mismatch')
        self.assertTrue(report['alignment']['duration_mismatch'])
        self.assertEqual(report['alignment']['segment_frame_count_difference'], 0)
        self.assertEqual(report['alignment']['duration_difference_rational_seconds'],
                         {'numerator': -1, 'denominator': 40})
        self.assertEqual(report['alignment']['duration_difference_seconds'], -0.025)

    def test_uses_explicit_recorded_original_end_boundary(self):
        report = self.boundary_report(end=3, output=self.root/'interior-pair')
        self.assertEqual(report['original_boundary']['start_sample_frame'], 0)
        self.assertEqual(report['original_boundary']['end_sample_frame_exclusive'], 25)
        self.assertEqual(report['segments']['original']['sample_frames'], 25)

    def test_fixed_tick_native_fallback_is_explicit_in_report(self):
        report = extract_route_audio_boundaries(
            self.original, self.original_index, 1, 'stop', self.native, 1200,
            self.root/'fixed-pair', native_first_tick=3, ticks=2)
        self.assertEqual(report['native_boundary']['scope'],
                         'explicit_fixed_tick_sample_rate_arithmetic')
        self.assertEqual(report['native_boundary']['start_sample_frame'], 40)
        self.assertEqual(report['native_boundary']['end_sample_frame_exclusive'], 80)

    def test_auto_prefers_adjacent_native_frame_index(self):
        frame_index = self.root/'native.png.frames'/'manifest.json'
        frame_index.parent.mkdir()
        frame_index.write_text(self.native_index.read_text())
        report = extract_route_audio_boundaries(
            self.original, self.original_index, 1, 'stop', self.native, 1200,
            self.root/'auto-index-pair', native_start_tick=3, native_end_tick=5)
        self.assertEqual(report['native_boundary']['scope'],
                         'recorded_native_capture_boundaries')
        self.assertEqual(report['sources']['manifests']['native_frame_index']['path'],
                         str(frame_index))

    def test_rejects_malformed_nonmonotone_and_out_of_range_original_indexes(self):
        base = json.loads(self.original_index.read_text())
        variants = []
        malformed = dict(base)
        malformed['vblanks'] = 'not-a-list'
        variants.append((malformed, 'no vblank boundary rows'))
        nonmonotone = json.loads(json.dumps(base))
        nonmonotone['vblanks'][2]['sample_frame_offset'] = 9
        variants.append((nonmonotone, 'sample offsets must be nondecreasing'))
        duplicate = json.loads(json.dumps(base))
        duplicate['vblanks'][1]['vblank_start'] = 1
        variants.append((duplicate, 'strictly consecutive'))
        out_of_range = json.loads(json.dumps(base))
        out_of_range['vblanks'][1]['sample_frame_offset'] = 31
        variants.append((out_of_range, 'exceeds total_sample_frames'))
        out_of_range_vblank = json.loads(json.dumps(base))
        out_of_range_vblank['vblanks'].append(
            {'vblank_start': 4, 'sample_frame_offset': 30})
        variants.append((out_of_range_vblank, 'exceeds successful run frame count'))
        for n, (index, message) in enumerate(variants):
            with self.subTest(index=n):
                self.original_index.write_text(json.dumps(index))
                with self.assertRaisesRegex(ValueError, message):
                    self.boundary_report(output=self.root/f'bad-index-{n}')

    def test_rejects_index_wav_extent_and_boundary_selector_mismatches(self):
        index = json.loads(self.original_index.read_text())
        index['total_sample_frames'] = 29
        self.original_index.write_text(json.dumps(index))
        with self.assertRaisesRegex(ValueError, 'index has 29 frames but WAV has 30'):
            self.boundary_report(output=self.root/'bad-extent')

        self.original_index.write_text(json.dumps({
            'sample_rate': 600, 'channels': 2, 'total_sample_frames': 30,
            'boundary': 'OnNewFrame', 'scope': 'block buffered',
            'vblanks': [
                {'vblank_start': 1, 'sample_frame_offset': 0},
                {'vblank_start': 2, 'sample_frame_offset': 10},
                {'vblank_start': 3, 'sample_frame_offset': 25},
            ],
        }))
        with self.assertRaisesRegex(ValueError, 'start vblank is not recorded'):
            self.boundary_report(start=4, output=self.root/'missing-boundary')

        correct = json.loads(self.original_index.read_text())
        correct['sample_rate'] = 44100
        self.original_index.write_text(json.dumps(correct))
        with self.assertRaisesRegex(ValueError, 'sample_rate disagrees with WAV'):
            self.boundary_report(output=self.root/'bad-index-rate')

    def test_rejects_bad_native_boundary_index_offsets(self):
        malformed = json.loads(self.native_index.read_text())
        malformed['captures'][1]['audio_sample_frame_offset'] = 19
        bad = self.root/'bad-native-index.json'
        bad.write_text(json.dumps(malformed))
        with self.assertRaisesRegex(ValueError, 'sample offsets must be nondecreasing'):
            self.boundary_report(native_index=bad, output=self.root/'bad-native-offsets')

        with self.assertRaisesRegex(ValueError, 'not recorded in frame index'):
            self.boundary_report(native_start=2, output=self.root/'missing-native-boundary')

        malformed = json.loads(self.native_index.read_text())
        malformed['captures'][1]['audio_sample_frame_offset'] = 101
        bad.write_text(json.dumps(malformed))
        with self.assertRaisesRegex(ValueError, 'exceeds native PCM extent'):
            self.boundary_report(native_index=bad, output=self.root/'native-offset-out-of-range')

        malformed = json.loads(self.native_index.read_text())
        malformed['complete'] = False
        bad.write_text(json.dumps(malformed))
        with self.assertRaisesRegex(ValueError, 'frame index is incomplete'):
            self.boundary_report(native_index=bad, output=self.root/'native-index-incomplete')

    def test_boundary_outputs_refuse_existing_files_and_symlinks(self):
        collision = self.root/'boundary-pair.native.wav'
        collision.write_bytes(b'keep')
        with self.assertRaisesRegex(ValueError, 'refusing existing output'):
            self.boundary_report()
        self.assertEqual(collision.read_bytes(), b'keep')
        collision.unlink()
        collision.symlink_to(self.root/'missing-boundary-target')
        with self.assertRaisesRegex(ValueError, 'refusing existing output'):
            self.boundary_report()

if __name__ == '__main__':
    unittest.main()
