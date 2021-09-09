import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from verify_shadow_transition import compare, fixture

class ShadowTransitionTests(unittest.TestCase):
    def test_observation_extent_identity_and_every_word(self):
        data = fixture()[:120]
        output = b''.join(struct.pack('<22I', 0, step, *([0]*20)) for step in range(49))
        self.assertEqual(compare(output, output, data), (49, 1078))
        for word in range(22):
            changed = bytearray(output)
            changed[(22*24+word)*4] ^= 1
            with self.assertRaises(ValueError):compare(output, changed, data)
        with self.assertRaisesRegex(ValueError, 'identity'):
            changed = bytearray(output);changed[4] ^= 1
            compare(changed, changed, data)
        for changed in (output[:-4], output+output[:88]):
            with self.assertRaisesRegex(ValueError, 'extent'):compare(changed, changed, data)

    def test_bad_fixture_and_duplicate_cases(self):
        data = fixture()[:120]
        for invalid in (b'', data[:-4], data*33):
            with self.assertRaisesRegex(ValueError, 'fixture extent'):compare(b'', b'', invalid)
        for word,value in ((1,6),(6,65536),(7,2),(9,129)):
            invalid = bytearray(data);struct.pack_into('<I',invalid,word*4,value)
            with self.assertRaisesRegex(ValueError, 'fixture scope'):compare(b'', b'', invalid)
        with self.assertRaisesRegex(ValueError, 'fixture scope'):compare(b'', b'', data*2)
