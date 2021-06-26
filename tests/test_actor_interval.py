import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from diagnose_actor_interval import compare, records

def row(frame, *, original, channels=4, textures=0):
    header = (frame, 0x51b0, 0x80, 0x100, 38 if original else 2, channels,
              999 if original else textures, 0x133a98 if original else 1)
    return header + (0,) * (channels * 4 + (0 if original else textures))

class ActorIntervalTests(unittest.TestCase):
    def test_missing_duplicate_and_shifted_observations_are_rejected(self):
        original = [row(i, original=True) for i in range(3)]
        native = [row(i, original=False) for i in range(9, 12)]
        run = lambda a, b: compare(a, b, instance=38, anchor=10, frames=3)
        self.assertTrue(run(original, native)['all_compared_words_equal'])
        for bad in [native[:2], native[:1] + native, native[::-1],
                    [row(i, original=False) for i in range(10, 13)]]:
            with self.assertRaises(ValueError):
                run(original, bad)
        with self.assertRaises(ValueError):
            run(original[:1] + original, native)
        changed = list(native[1])
        changed[12] = 0x3f800000
        report = run(original, [native[0], tuple(changed), native[2]])
        self.assertFalse(report['all_compared_words_equal'])
        self.assertEqual(report['difference_frames']['root_words'], [1])
        self.assertEqual(report['different_words']['root_words'], 1)

    def test_variable_channel_stride_and_truncation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'actor.bin'
            for native in [True, False]:
                expected = [row(1, original=not native, channels=4, textures=2),
                            row(2, original=not native, channels=9, textures=3)]
                data = b''.join(struct.pack('<' + str(len(r)) + 'I', *r) for r in expected)
                path.write_bytes(data)
                self.assertEqual(records(path, native=native), expected)
                path.write_bytes(data[:-1])
                with self.assertRaises(ValueError):
                    records(path, native=native)

if __name__ == '__main__':
    unittest.main()
