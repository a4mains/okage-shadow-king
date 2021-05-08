import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]/'tools'))
from verify_music_channels import compare, fixture

class MusicChannelAuditorTests(unittest.TestCase):
    def test_reviewed_requests(self):
        for kind, sha in [('ee','2888e72df81b2bcdf77c3910ea269f15041b90efed8c14367d756e1b5b1bb616'),
                          ('iop','0a6290b0d416ba0c802834663b424f1a984e551c696c39d54f32b9b12e7611ba')]:
            self.assertEqual(hashlib.sha256(fixture(kind)).hexdigest(), sha)

    def test_missing_and_changed_driver_state(self):
        for kind, words in [('ee',680), ('iop',10240)]:
            expected = bytes(words*4)
            self.assertEqual(compare(expected, expected, kind), words)
            for bad in (expected[:-4], expected+b'\0\0\0\0'):
                with self.assertRaises(ValueError):
                    compare(expected, bad, kind)
            for word in (0,16,33,63,words-1):
                bad = bytearray(expected)
                bad[word*4] = 1
                with self.subTest(kind=kind, word=word), self.assertRaises(ValueError):
                    compare(expected, bad, kind)
