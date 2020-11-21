import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('inspect_disc', Path(__file__).parents[1] / 'tools/inspect_disc.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

def record(name, lba, size, flags=0):
    b = bytearray(33 + len(name) + (len(name) % 2 == 0))
    b[0] = len(b)
    struct.pack_into('<I', b, 2, lba)
    struct.pack_into('>I', b, 6, lba)
    struct.pack_into('<I', b, 10, size)
    struct.pack_into('>I', b, 14, size)
    b[25], b[32] = flags, len(name)
    b[33:33 + len(name)] = name
    return b

def elf():
    b = bytearray(84)
    b[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHIIIIIHHHHHH', b, 16, 2, 8, 1, 0x100008, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    struct.pack_into('<8I', b, 52, 1, 0, 0x100000, 0, 84, 128, 5, 4096)
    return b

def iso():
    b = bytearray(24 * 2048)
    b[16 * 2048:16 * 2048 + 7] = b'\x01CD001\x01'
    b[16 * 2048 + 40:16 * 2048 + 72] = b'TEST'.ljust(32)
    struct.pack_into('<H', b, 16 * 2048 + 128, 2048)
    r = record(b'\0', 20, 2048, 2)
    b[16 * 2048 + 156:16 * 2048 + 156 + len(r)] = r
    cnf = b'BOOT2 = cdrom0:\\SCUS_971.29;1\n'
    directory = r + record(b'\1', 20, 2048, 2) + record(b'SYSTEM.CNF;1', 21, len(cnf)) + record(b'SCUS_971.29;1', 22, len(elf()))
    b[20 * 2048:20 * 2048 + len(directory)] = directory
    b[21 * 2048:21 * 2048 + len(cnf)] = cnf
    b[22 * 2048:22 * 2048 + len(elf())] = elf()
    return b

def raw(cooked, mode):
    out = bytearray()
    for i in range(0, len(cooked), 2048):
        b = bytearray(2352)
        b[:12] = b'\x00' + b'\xff' * 10 + b'\x00'
        b[15] = mode
        offset = 24 if mode == 2 else 16
        b[offset:offset + 2048] = cooked[i:i + 2048]
        out.extend(b)
    return out

class InspectionTests(unittest.TestCase):
    def test_all_sector_layouts_and_exact_extraction(self):
        for mode in (0, 1, 2):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                p = Path(tmp) / 'disc.bin'
                p.write_bytes(iso() if mode == 0 else raw(iso(), mode))
                out = Path(tmp) / 'extracted'
                r = mod.inspect(p, out)
                self.assertEqual(r['boot_executable'], 'SCUS_971.29')
                self.assertEqual(r['elf']['entry'], '0x00100008')
                self.assertEqual(len(r['files']), 2)
                self.assertEqual((out / 'SCUS_971.29').read_bytes(), elf())
                self.assertEqual(r, mod.inspect(p, out))
                (out / 'SCUS_971.29').write_bytes(b'changed')
                with self.assertRaisesRegex(ValueError, 'overwrite'):
                    mod.inspect(p, out)

    def test_rejects_form2_and_truncated_disc(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'disc.bin'
            b = raw(iso(), 2)
            b[20 * 2352 + 18] = b[20 * 2352 + 22] = 0x20
            p.write_bytes(b)
            with self.assertRaisesRegex(ValueError, 'Form 1'):
                mod.inspect(p)
            p.write_bytes(b[:-1])
            with self.assertRaisesRegex(ValueError, 'Truncated'):
                mod.Disc(p)

    def test_rejects_bad_elf_ranges(self):
        b = elf()
        struct.pack_into('<I', b, 52 + 16, 4096)
        with self.assertRaisesRegex(ValueError, 'outside file'):
            mod.inspect_elf(b)
        with self.assertRaisesRegex(ValueError, 'ELF32'):
            mod.inspect_elf(b'not an executable')

    def test_rejects_directory_traversal_and_cycles(self):
        for name, flags in ((b'../BAD;1', 0), (b'CYCLE', 2)):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                p = Path(tmp) / 'disc.iso'
                b = iso()
                r = record(name, 20, 2048, flags)
                b[20 * 2048:21 * 2048] = bytes(r).ljust(2048, b'\0')
                p.write_bytes(b)
                d = mod.Disc(p)
                try:
                    with self.assertRaises(ValueError):
                        d.inventory()
                finally:
                    d.close()

if __name__ == '__main__':
    unittest.main()
