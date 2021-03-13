import importlib.util
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
spec = importlib.util.spec_from_file_location('verify_enemy', Path(__file__).parents[1] / 'tools/verify_enemy.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

class EnemyComparisonTests(unittest.TestCase):
    def test_rejects_truncation_and_every_changed_word(self):
        names, data = mod.cases()
        mod.compare(data, data, len(names))
        with self.assertRaises(ValueError):
            mod.compare(data, data[:-144], len(names))
        for word in range(36):
            altered = bytearray(data)
            altered[word * 4] ^= 1
            with self.subTest(word=word), self.assertRaises(ValueError):
                mod.compare(data, altered, len(names))
