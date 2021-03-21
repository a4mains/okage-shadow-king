import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from verify_destination_setup import compare

class DestinationSetupTests(unittest.TestCase):
    def setUp(self):
        self.ram = bytearray(32*1024*1024)
        for address, value in [(0x2d9bf4, 0x100000), (0x100014, 0x200000),
                               (0x200044, 1), (0x20004c, 1), (0x100410, 60),
                               (0x100080, 1), (0x2d9ab4, 10), (0x2d9aa8, 10), (0x2d9aac, 26)]:
            struct.pack_into('<I', self.ram, address, value)
        self.projection = struct.pack('<4I', 1, 2, 3, 4)
        for index, value in zip((0, 1, 2, 6), (1, 2, 3, 4)):
            struct.pack_into('<I', self.ram, 0x332220+index*4, value)
        self.log = ('Native destination setup: scene 70; party 0; followers 0; music 51; slots 51,-1\n'
                    'Native destination setup: scene 1; party 1; followers 0; music 10; slots 10,26\n')

    def test_exact(self):
        self.assertEqual(compare(self.log, self.ram, self.projection)['compared_words'], 10)

    def test_wrong_route_or_identity(self):
        for log in (self.log.splitlines()[1], self.log+self.log, self.log.replace('party 1', 'party 0'),
                    self.log.replace('followers 0; music 10', 'followers 1; music 10'),
                    self.log.replace('slots 10,26', 'slots 26,10')):
            with self.assertRaises(ValueError):
                compare(log, self.ram, self.projection)

    def test_projection_and_extent(self):
        for ram, projection in ((self.ram, b'\0'*16), (self.ram[:-1], self.projection),
                                (self.ram, self.projection[:-1])):
            with self.assertRaises(ValueError):
                compare(self.log, ram, projection)

    def test_wrong_reference_scene(self):
        struct.pack_into('<I', self.ram, 0x200044, 70)
        with self.assertRaises(ValueError):
            compare(self.log, self.ram, self.projection)
