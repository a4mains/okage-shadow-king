import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('import_battle_data', Path(__file__).resolve().parents[1]/'tools/import_battle_data.py')
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)

class SyntheticImage:
    def __init__(self):
        self.data = bytearray(0x250000)
        struct.pack_into('<4I', self.data, 0x246d20, 0x15b580, 0x15b9a0, 0x15c138, 0x15c1b0)
        for i in range(256):
            struct.pack_into('<Ii', self.data, 0x246520+i*8, 0, -1)
        # Nonconsecutive IDs test directory preservation, not current retail data.
        struct.pack_into('<II', self.data, 0x246520+8, 0x244bf0, 2)
        struct.pack_into('<II', self.data, 0x246520+255*8, 0x244c08, 1)
        self.records = (struct.pack('<hbbII', -32768, -128, 127, 0x80000000, 0x7fc12345) +
                        struct.pack('<hbbff', -1, -1, 0, 1.25, -2.5) +
                        struct.pack('<hbbff', 32767, 12, -3, 3.0, 4.0))
        self.data[0x244bf0:0x244bf0+len(self.records)] = self.records
        for i in range(1, 24):
            address = 0x237000+i*128
            struct.pack_into('<I', self.data, 0x1fce68+i*4, address)
            value = f'synthetic_{i}.xpf\0'.encode()
            self.data[address:address+len(value)] = value

    def read(self, address, size):
        if address < 0 or address+size > len(self.data):
            raise ValueError('Invalid synthetic range')
        return bytes(self.data[address:address+size])

class BattleImportTests(unittest.TestCase):
    def extract(self, image):
        return m.extract_tables(image, bytes.fromhex(m.ELF_SHA256))

    def test_pointer_free_ordered_exact_entries(self):
        image = SyntheticImage()
        data = self.extract(image)
        self.assertEqual(m.HEADER.unpack_from(data), (b'OKBTDAT1', 1, 256, 23, 3, bytes.fromhex(m.ELF_SHA256)))
        self.assertEqual(struct.unpack_from('<II', data, 56), (0, 0xffffffff))
        self.assertEqual(struct.unpack_from('<II', data, 64), (0, 2))
        self.assertEqual(struct.unpack_from('<II', data, 56+255*8), (2, 1))
        self.assertEqual(data[56+256*8:56+256*8+64], b'btldata/beta2/field/synthetic_1.xpf'.ljust(64, b'\0'))
        self.assertEqual(data[-36:], image.records)
        self.assertEqual(len(data), 56+256*8+23*64+36)
        self.assertEqual(self.extract(image), data)

    def test_pinned_asset_identity(self):
        for data in (b'', b'\x7fELF'+bytes(100), b'wrong retail revision'):
            with self.subTest(data=data), self.assertRaisesRegex(ValueError, 'SHA256'):
                m.import_elf(data)

    def test_rejects_wrong_extent_and_bad_pairs(self):
        for pointer, count in ((0, 1), (0x244bf0, -1), (0x244bf0, 0),
                               (0x244bf0, 11), (0x244bf1, 1), (0x24651c, 1)):
            image = SyntheticImage()
            struct.pack_into('<Ii', image.data, 0x246528, pointer, count)
            with self.subTest(pointer=pointer, count=count), self.assertRaises(ValueError):
                self.extract(image)
        image = SyntheticImage()
        image.data[0x246d20] ^= 1
        with self.assertRaisesRegex(ValueError, 'descriptor'):
            self.extract(image)

    def test_rejects_unsafe_and_unterminated_stage_names(self):
        for name in (b'../bad.xpf\0', b'bad/name.xpf\0', b'bad.XPF\0', b'.xpf\0', b'a'*64):
            image = SyntheticImage()
            image.data[0x237080:0x237080+64] = name.ljust(64, b'\0')
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.extract(image)

    def test_output_preserves_existing_and_refuses_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'nested'/'data.bin'
            m.write_output(path, b'first')
            m.write_output(path, b'first')
            with self.assertRaises(ValueError):
                m.write_output(path, b'second')
            self.assertEqual(path.read_bytes(), b'first')
            link = Path(tmp)/'link'
            link.symlink_to(path)
            with self.assertRaises(ValueError):
                m.write_output(link, b'first')

    def test_elf_file_backing_and_ambiguous_segments(self):
        data = bytearray(0x200)
        data[:7] = b'\x7fELF\x01\x01\x01'
        struct.pack_into('<H', data, 18, 8)
        struct.pack_into('<I', data, 28, 52)
        struct.pack_into('<HH', data, 42, 32, 1)
        struct.pack_into('<8I', data, 52, 1, 0x100, 0x1000, 0, 16, 32, 0, 0)
        data[0x100:0x110] = bytes(range(16))
        image = m.ElfImage(bytes(data))
        self.assertEqual(image.read(0x1001, 3), bytes([1, 2, 3]))
        for address, size in ((0xfff, 1), (0x100f, 2), (0x1010, 1)):
            with self.assertRaises(ValueError):
                image.read(address, size)
        struct.pack_into('<H', data, 44, 2)
        data[84:116] = data[52:84]
        with self.assertRaisesRegex(ValueError, 'ambiguous'):
            m.ElfImage(bytes(data)).read(0x1000, 1)
        struct.pack_into('<I', data, 52+16, 0x1000)
        with self.assertRaises(ValueError):
            m.ElfImage(bytes(data))

if __name__ == '__main__':
    unittest.main()
