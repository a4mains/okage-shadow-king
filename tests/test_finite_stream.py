import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from verify_finite_stream import audit_original, finite_sample

class FiniteStreamAuditTests(unittest.TestCase):
    def setUp(self):
        self.source = b'\x0c\x07'+bytes(14)
        self.raw = bytes(64)+b'\x00\x01'+bytes(62)
        self.raw = b'\x0c'+self.raw[1:]
        self.commands = b''.join(struct.pack('<IIIBB2xI', 161, 0, 8, 0, eof, count)
                                 for eof, count in [(0, 0), (0, 64), (1, 64)])
        self.context = struct.pack('<16I', 0, 256, 1, 0, 0, 0x40000, 0x100000,
                                   64, 64, 0, 0, 4096, 64, 0, 64, 0)

    def test_valid_and_each_output_mutation(self):
        self.assertEqual(audit_original(self.source, self.raw, self.commands, self.context), self.raw)
        fields = [self.raw, self.commands, self.context]
        for field in range(3):
            for at in range(len(fields[field])):
                modified = list(fields)
                b = bytearray(modified[field]);b[at] ^= 1;modified[field] = bytes(b)
                with self.assertRaises(ValueError):
                    audit_original(self.source, *modified)
            with self.assertRaises(ValueError):
                modified = list(fields);modified[field] = modified[field][:-1]
                audit_original(self.source, *modified)

    def test_extent_and_missing_stop(self):
        for value in [b'', bytes(15), bytes(512*1024+16)]:
            with self.assertRaises(ValueError):
                finite_sample(value)
        with self.assertRaises(ValueError):
            audit_original(self.source, self.raw[:64], self.commands, self.context)

if __name__ == '__main__':
    unittest.main()
