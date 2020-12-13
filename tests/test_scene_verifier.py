import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('scene_verifier', Path(__file__).resolve().parents[1]/'tools/verify_scene.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class SceneVerifierTests(unittest.TestCase):
    def test_unrequested_storage_is_not_reference_data(self):
        a = struct.pack('<32f', *([1.0]*16+[float('nan')]*16))
        b = struct.pack('<32f', *([1.0]*32))
        self.assertEqual(module.compare(a, b, [0])['matrices'], 1)
        with self.assertRaises(ValueError):
            module.compare(a, b, [1])

    def test_rejects_changed_matrix_and_extent(self):
        a = struct.pack('<16f', *([1.0]*16))
        for b in [a[:-4], struct.pack('<16f', *([1.0]*15+[2.0])),
                  struct.pack('<16f', *([1.0]*15+[float('inf')]))]:
            with self.assertRaises(ValueError):
                module.compare(a, b)
        for selection in [[], [1], [-1], [0, 0]]:
            with self.assertRaises(ValueError):
                module.compare(a, a, selection)
