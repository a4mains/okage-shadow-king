import pathlib
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import import_battle_ui_commands as importer

class Image:
    def __init__(self, values=None):
        self.values = values or {
            0x247ba0: struct.pack('<2i', 5, 19),
            0x247ba8: struct.pack('<2i', 20, 6),
            0x247bb0: struct.pack('<4i', 2, 0, 4, 6),
            0x200b78: struct.pack('<4i', 12, 24, 6, 18),
            0x200b88: struct.pack('<4i', -1000, 0, 512, 2048),
        }
        self.reads = []

    def read(self, address, size):
        self.reads.append((address, size))
        data = self.values[address]
        if len(data) != size:
            raise ValueError('fixture extent mismatch')
        return data

class ImportBattleUiCommandTests(unittest.TestCase):
    def test_structural_mapping_and_payload_order(self):
        image = Image()
        digest = bytes.fromhex(importer.ELF_SHA256)
        payload, metadata = importer.extract_payload(image, digest)
        self.assertEqual(image.reads, [(0x247ba0, 8), (0x247ba8, 8),
                                      (0x247bb0, 16), (0x200b78, 16), (0x200b88, 16)])
        self.assertEqual(len(payload), 64)
        self.assertEqual(struct.unpack('<16i', payload),
                         (5, 19, 20, 6, 2, 0, 4, 6, 12, 24, 6, 18, -1000, 0, 512, 2048))
        self.assertEqual(metadata['shared_defaults']['metrics']['ownership'],
                         'authored ELF initial contents; runtime global may mutate')

    def test_unexpected_values_and_invalid_digest_rejected(self):
        values = Image().values.copy()
        values[0x247bb0] = struct.pack('<4i', 2, 0, 4, 7)
        with self.assertRaises(ValueError):
            importer.extract_payload(Image(values), bytes(32))
        with self.assertRaises(ValueError):
            importer.extract_payload(Image(), b'bad')

    def test_import_elf_hash_pinned(self):
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            importer.import_elf(b'not the owned ELF')

    def test_cli_writes_exact_artifact_and_refuses_overwrite(self):
        # Exercise the CLI's fresh-output policy independently of retail input.
        artifact = importer.HEADER.pack(importer.MAGIC, 1, 64,
                                        bytes.fromhex(importer.ELF_SHA256), bytes(32)) + bytes(64)
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.elf'
            source.write_bytes(b'fixture ELF input')
            output = pathlib.Path(directory) / 'data.bin'
            manifest = pathlib.Path(directory) / 'manifest.json'
            with mock.patch.object(importer, 'import_elf', return_value=(artifact, {'schema': 'fixture'})):
                self.assertEqual(importer.main(['--elf', str(source), '--output', str(output),
                                                '--manifest', str(manifest)]), 0)
            self.assertEqual(output.read_bytes(), artifact)
            self.assertIn('fixture', manifest.read_text())
            with self.assertRaises(ValueError):
                importer.main(['--elf', str(source), '--output', str(output), '--manifest', str(manifest)])

if __name__ == '__main__':
    unittest.main()
