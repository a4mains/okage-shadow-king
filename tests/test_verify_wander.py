import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_wander import (ENTRY_PC, EXIT_PC, SCHEMA, WanderFormatError,
                           compare, load_native, load_original)

def snapshot(frame, phase, pc, *, counter=7, rng=11, velocity=None, channels=None):
    if velocity is None:
        velocity = [1, 2, 3, 4]
    if channels is None:
        channels = [10, 20, 30, 40]
    return {
        'frame': frame, 'phase': phase, 'pc': pc,
        'field_npc_count': 3, 'alias13_instance': 3,
        'job_34': 1, 'job_38': 1, 'job_40': 3, 'job_58': 1,
        'job': 0x123400, 'actor_object': 0x567800,
        'first_channel': 40, 'channel_group': 2,
        'job_3c': counter, 'rng_state': rng, 'job_20': velocity,
        'channel_count': len(channels) // 4, 'channels_u32': channels,
    }

def original_pair(pair_no, frame):
    return {'pair': pair_no,
            'entry': snapshot(frame, 'pre', ENTRY_PC),
            'exit': snapshot(frame, 'post', EXIT_PC)}

def native_record(tick, phase, *, counter=7, rng=11, velocity=None, channels=None,
                  scene=68, alias=-13):
    if velocity is None:
        velocity = [1, 2, 3, 4]
    if channels is None:
        channels = [10, 20, 30, 40]
    header = struct.pack('<11I', tick, scene, alias & 0xffffffff, phase,
                         counter, rng, len(channels) // 4, *velocity)
    return header + struct.pack('<%dI' % len(channels), *channels)

class WanderVerifierTests(unittest.TestCase):
    def write_original(self, directory, pairs, header=None):
        path = Path(directory) / 'original.jsonl'
        if header is None:
            header = {'schema': SCHEMA, 'version': 1, 'entry_pc': ENTRY_PC, 'exit_pc': EXIT_PC}
        path.write_text('\n'.join([json.dumps(header), *(json.dumps(p) for p in pairs)]) + '\n')
        return path

    def write_native(self, directory, rows):
        path = Path(directory) / 'native.bin'
        path.write_bytes(b''.join(rows))
        return path

    def test_exact_fixed_mapping_and_raw_difference_categories(self):
        with tempfile.TemporaryDirectory() as directory:
            original = self.write_original(directory, [original_pair(1, 1)])
            native = self.write_native(directory, [
                native_record(101, 0),
                native_record(101, 1, rng=12, channels=[10, 20, 31, 40]),
            ])
            header, snapshots, pairs = load_original(original, 2)
            records, incomplete = load_native(native)
            report = compare(snapshots, records, first_native_tick=100, requested_ticks=2)
            self.assertEqual((header['schema'], pairs), (SCHEMA, 1))
            self.assertFalse(incomplete)
            self.assertEqual(report['matched_callback_snapshots'], 2)
            self.assertEqual(report['fixed_mapping']['rule'], 'native_tick = first_native_tick + original.frame')
            self.assertEqual(report['categories']['rng']['raw_words_differing'], 1)
            self.assertEqual(report['categories']['rng']['first_difference']['tick'], 101)
            self.assertEqual(report['categories']['channels']['raw_words_differing'], 1)
            self.assertEqual(report['categories']['channels']['first_difference']['index'], 2)
            self.assertEqual(report['result'], 'different')

    def test_zero_based_first_and_last_frames_map_to_inclusive_native_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            original = self.write_original(directory, [original_pair(1, 0), original_pair(2, 2)])
            _, snapshots, pairs = load_original(original, 3)
            native = [native_record(tick, phase) for tick in (100, 102) for phase in (0, 1)]
            records, _ = load_native(self.write_native(directory, native))
            report = compare(snapshots, records, first_native_tick=100, requested_ticks=3)
            self.assertEqual(pairs, 2)
            self.assertEqual(report['fixed_mapping']['original_frame_min_in_requested_interval'], 0)
            self.assertEqual(report['fixed_mapping']['original_frame_max_in_requested_interval'], 2)
            self.assertEqual(report['fixed_mapping']['mapped_native_tick_min'], 100)
            self.assertEqual(report['fixed_mapping']['mapped_native_tick_max'], 102)
            self.assertEqual(report['matched_callback_snapshots'], 4)
            outside = self.write_original(directory, [original_pair(1, 3)])
            with self.assertRaisesRegex(WanderFormatError, 'outside requested interval'):
                load_original(outside, 3)

    def test_original_header_must_be_an_object(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(WanderFormatError, 'header must be a JSON object'):
                load_original(self.write_original(directory, [], header=[]), 3)

    def test_original_pair_requires_stable_job_actor_and_channel_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            for field, value in [('job', 0x123401), ('actor_object', 0x567801),
                                 ('first_channel', 41), ('channel_group', 3)]:
                with self.subTest(field=field):
                    pair = original_pair(1, 0)
                    pair['exit'][field] = value
                    with self.assertRaisesRegex(WanderFormatError, 'changed job/actor/channel provenance'):
                        load_original(self.write_original(directory, [pair]), 3)
            pair = original_pair(1, 0)
            pair['exit']['channel_count'] = 2
            pair['exit']['channels_u32'] = [10, 20, 30, 40, 50, 60, 70, 80]
            with self.assertRaisesRegex(WanderFormatError, 'changed job/actor/channel provenance: channel_count'):
                load_original(self.write_original(directory, [pair]), 3)

    def test_missing_and_native_only_callbacks_are_separate_from_activation(self):
        with tempfile.TemporaryDirectory() as directory:
            original = self.write_original(directory, [original_pair(1, 2)])
            header, snapshots, _ = load_original(original, 3)
            del header
            native_path = self.write_native(directory, [native_record(101, 0), native_record(101, 1),
                                                         native_record(102, 0)])
            native, _ = load_native(native_path)
            report = compare(snapshots, native, first_native_tick=100, requested_ticks=3)
            self.assertEqual(report['missing_native_observations'], 1)
            self.assertEqual(report['missing_native_examples'], [{'tick': 102, 'phase': 'post'}])
            self.assertEqual(report['native_only_callbacks_in_mapped_interval'], 2)
            self.assertEqual(report['activation_mismatch_tick_count'], 1)
            self.assertEqual(report['activation_mismatch_ranges'], [
                {'first_tick': 101, 'last_tick': 101,
                 'original_active': False, 'native_active': True},
            ])

    def test_original_pairing_order_and_channel_extent_are_validated(self):
        with tempfile.TemporaryDirectory() as directory:
            pair = original_pair(1, 1)
            pair['exit']['frame'] = 2
            with self.assertRaisesRegex(WanderFormatError, 'frame is inconsistent'):
                load_original(self.write_original(directory, [pair]), 3)
            pair = original_pair(2, 1)
            with self.assertRaisesRegex(WanderFormatError, 'numbering/order'):
                load_original(self.write_original(directory, [pair]), 3)
            pair = original_pair(1, 1)
            pair['entry']['channels_u32'].pop()
            with self.assertRaisesRegex(WanderFormatError, 'channel extent'):
                load_original(self.write_original(directory, [pair]), 3)

    def test_native_truncation_scene_alias_duplicate_order_and_incomplete_pair(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_native(directory, [native_record(101, 0)[:-1]])
            with self.assertRaisesRegex(WanderFormatError, 'truncated native channel payload'):
                load_native(path)
            for rows, message in [
                ([native_record(101, 0, scene=1)], 'scene/alias'),
                ([native_record(101, 0, alias=-12)], 'scene/alias'),
                ([native_record(101, 2)], 'invalid phase'),
                ([native_record(101, 0), native_record(101, 0)], 'duplicate native callback'),
                ([native_record(101, 1), native_record(101, 0)], 'out of tick/phase order'),
            ]:
                with self.subTest(message=message):
                    with self.assertRaisesRegex(WanderFormatError, message):
                        load_native(self.write_native(directory, rows))
            records, incomplete = load_native(self.write_native(directory, [native_record(101, 0)]))
            self.assertEqual(len(records), 1)
            self.assertEqual(incomplete, {101: [1]})

if __name__ == '__main__':
    unittest.main()
