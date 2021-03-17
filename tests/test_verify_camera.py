import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).parents[1]
sys.path.insert(0, str(ROOT/'tools'))

def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT/'tools'/(name+'.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

class CameraAuditorTests(unittest.TestCase):
    def test_collision_auditor_rejects_missing_or_changed_output(self):
        self.check_auditor(load('verify_camera_collision').compare, 88)

    def test_manager_auditor_rejects_missing_or_changed_output(self):
        self.check_auditor(load('verify_exterior_camera').compare, 184)

    def check_auditor(self, compare, width):
        expected = bytes(width*2)
        compare(expected, expected, 2)
        with self.assertRaises(ValueError):
            compare(expected, expected[:-4], 2)
        for word in range(width//4):
            actual = bytearray(expected)
            actual[width+word*4] = 1
            with self.subTest(word=word), self.assertRaises(ValueError):
                compare(expected, actual, 2)
