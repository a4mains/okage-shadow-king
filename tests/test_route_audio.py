import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from compare_route_audio import compare_route_audio

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

if __name__ == '__main__':
    unittest.main()
