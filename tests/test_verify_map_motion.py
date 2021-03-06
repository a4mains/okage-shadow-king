import importlib.util
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
spec = importlib.util.spec_from_file_location('verify_map_motion', Path(__file__).parents[1] / 'tools/verify_map_motion.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

def pack(records):
    return b''.join(struct.pack('<' + 'I' * len(row), *row) for row in records)

def fixture():
    def sample(frame, boundary=mod.FIELD, motion=0):
        return [frame, 100, 40, 1, 18, 10, motion, boundary] + [0] * 40
    first = [sample(139, mod.REQUEST, 1)]
    for frame in range(140, 260):
        first.append(sample(frame))
        if frame == 223:
            first.append(sample(frame, mod.REQUEST, 3))
    continuation = [sample(frame) for frame in range(60)]
    native = [[frame, 40, 1, 1, 18, 10] + [0] * 40 for frame in range(180)]
    return first, continuation, native

class MotionAuditTests(unittest.TestCase):
    def test_exact_fixture_and_request_alignment(self):
        first, continuation, native = fixture()
        samples, requests = mod.reference(pack(first), pack(continuation))
        self.assertEqual(requests, [(0, 1), (84, 3)])
        self.assertEqual(mod.compare(samples, pack(native)), 180)

    def test_rejects_truncation_and_missing_updates(self):
        first, continuation, native = fixture()
        with self.assertRaises(ValueError):
            mod.reference(pack(first)[:-1], pack(continuation))
        with self.assertRaises(ValueError):
            mod.reference(pack(first), pack(continuation[:-1]))
        samples, _ = mod.reference(pack(first), pack(continuation))
        with self.assertRaises(ValueError):
            mod.compare(samples, pack(native[:-1]))

    def test_rejects_corruption_and_binding_mismatch(self):
        first, continuation, native = fixture()
        samples, _ = mod.reference(pack(first), pack(continuation))
        for word in (0, 1, 2, 3, 4, 5, 6, 45):
            with self.subTest(word=word):
                changed = [r.copy() for r in native]
                changed[84][word] ^= 1
                with self.assertRaises(ValueError):
                    mod.compare(samples, pack(changed))

    def test_rejects_shifted_request_or_observation(self):
        first, continuation, _ = fixture()
        for records, index in ((first, 0), (continuation, 0)):
            records[index][0] += 1
            with self.assertRaises(ValueError):
                mod.reference(pack(first), pack(continuation))
            records[index][0] -= 1
