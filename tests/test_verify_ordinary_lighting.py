import struct
import unittest
from verify_ordinary_lighting import compare, fixtures

class OrdinaryLightingVerifierTests(unittest.TestCase):
    def test_difference_keeps_record_and_word_boundary(self):
        original = bytes(272 * 2)
        native = bytearray(original)
        struct.pack_into('<I', native, 272 + 67 * 4, 0x4b000001)
        self.assertEqual(compare(original, native), [dict(record=1, word=67, original=0, native=0x4b000001)])

    def test_output_extent_is_required(self):
        for a, b in [(b'', b''), (bytes(4), bytes(4)), (bytes(272), bytes(544))]:
            with self.assertRaises(ValueError):
                compare(a, b)

    def test_fixture_protocol_and_modes(self):
        data = fixtures()
        self.assertEqual(len(data), 256 * 400)
        for i in range(256):
            self.assertEqual(struct.unpack_from('<4I', data, i * 400), (i, i % 4, 0, 0))
        self.assertEqual(data[16:400], data[416:800])
