from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_map_effect import compare

class MapEffectComparisonTests(unittest.TestCase):
    def fixture(self):
        rows = []
        for step in range(3):
            row = [0] * 381
            row[:5] = [step, 0, 25, 0x9000, 8]
            rows += row
        return struct.pack(f'<{len(rows)}I', *rows)

    def test_rejects_truncation_and_identity(self):
        data = self.fixture()
        compare(data, data, 25, 8, 1)
        for a, b in [(data[:-4], data), (data, data[:-4])]:
            with self.assertRaises(ValueError):
                compare(a, b, 25, 8, 1)
        bad = bytearray(data)
        struct.pack_into('<I', bad, 8, 34)
        with self.assertRaises(ValueError):
            compare(bad, bad, 25, 8, 1)

    def test_only_updated_particle_xyz_has_tolerance(self):
        data = self.fixture()
        for record in range(3):
            for word in range(381):
                bad = bytearray(data)
                at = (record * 381 + word) * 4
                struct.pack_into('<I', bad, at, struct.unpack_from('<I', bad, at)[0] ^ 1)
                element, channel = divmod(word - 29, 44)
                if record == 2 and 3 <= element < 8 and 24 <= channel < 27:
                    compare(data, bad, 25, 8, 1)
                else:
                    with self.assertRaises(ValueError):
                        compare(data, bad, 25, 8, 1)
        at = (2 * 381 + 29 + 3 * 44 + 24) * 4
        for value in (3e-6, float('nan'), float('inf')):
            bad = bytearray(data)
            struct.pack_into('<f', bad, at, value)
            with self.assertRaises(ValueError):
                compare(data, bad, 25, 8, 1)
