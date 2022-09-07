import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
SPEC = importlib.util.spec_from_file_location(
    'import_battle_resources', ROOT / 'tools/import_battle_resources.py')
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)

class SyntheticImage:
    def __init__(self):
        self.data = bytearray(0x240000)

    def write(self, address, data):
        self.data[address:address + len(data)] = data

    def read(self, address, size):
        if address < 0 or size < 0 or address + size > len(self.data):
            raise ValueError(f'Unmapped synthetic range {address:#x}+{size:#x}')
        return bytes(self.data[address:address + size])

def put_row(image, stage, offset, pointers):
    index = stage * 8 + 0x5c + offset
    image.write(m.CAMERA_TABLE + index * 12, struct.pack('<3I', *pointers))

class BattleResourceImporterTests(unittest.TestCase):
    def test_bundle_includes_command_artifact_and_current_hud_provenance(self):
        elf = b'synthetic ELF boundary; each owned sub-importer is independently tested'
        hud = b'OKBHUD2\0' + bytes(range(32))
        commands = b'OKBCMD1\0' + bytes(reversed(range(32)))
        hud_manifest = {'schema': 'okage-battle-hud-import-v2',
                        'artifact_sha256': m.sha256(hud)}
        command_manifest = {'schema': 'okage-battle-ui-command-import-v1',
                            'artifact_sha256': m.sha256(commands),
                            'shared_defaults': {'metrics': {'ownership': 'runtime global may mutate'}}}
        ruw_entry = {'path': m.RUW_PATH, 'lba': 42, 'size': 3}
        with mock.patch.multiple(m, ELF_SHA256=m.sha256(elf)), \
             mock.patch.object(m, 'import_elf', return_value=b'formation'), \
             mock.patch.object(m, 'ElfImage', return_value=object()), \
             mock.patch.object(m, 'extract_camera_tables', return_value=b'camera'), \
             mock.patch.object(m, 'import_ui_elf', return_value=(b'ui', {'version': 1})), \
             mock.patch.object(m, 'import_hud_elf', return_value=(hud, hud_manifest)) as hud_import, \
             mock.patch.object(m, 'import_commands_elf', return_value=(commands, command_manifest)) as command_import, \
             mock.patch.object(m, 'extract_owned_ruw', return_value=(b'ruw', ruw_entry)):
            artifacts, manifest = m.import_resources(elf, Path('owned-disc'))
        hud_import.assert_called_once_with(elf)
        command_import.assert_called_once_with(elf)
        self.assertEqual(set(artifacts), {'battle-data.bin', 'BTL.RUW', 'battle-camera.bin',
                                         'battle-ui.bin', 'battle-hud.bin', 'battle-ui-commands.bin'})
        self.assertEqual(artifacts['battle-ui-commands.bin'], commands)
        self.assertEqual(artifacts['battle-hud.bin'], hud)
        self.assertEqual(manifest['commands_format'], command_manifest)
        self.assertEqual(manifest['hud_format'], hud_manifest)
        self.assertEqual(manifest['sources']['elf_sha256'], m.sha256(elf))
        for name, data in artifacts.items():
            self.assertEqual(manifest['artifacts'][name], {'size': len(data), 'sha256': m.sha256(data)})
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / 'bundle'
            m.write_bundle(target, artifacts, manifest)
            self.assertEqual((target / 'battle-ui-commands.bin').read_bytes(), commands)
            self.assertEqual((target / 'battle-hud.bin').read_bytes(), hud)
            with self.assertRaisesRegex(ValueError, 'already exists'):
                m.write_bundle(target, {'battle-ui-commands.bin': b'corrupt'}, {})
            self.assertEqual((target / 'battle-ui-commands.bin').read_bytes(), commands)

    def test_command_validation_failure_does_not_create_partial_bundle(self):
        elf = b'controlled integration boundary'
        with mock.patch.object(m, 'ELF_SHA256', m.sha256(elf)), \
             mock.patch.object(m, 'import_elf', return_value=b'formation'), \
             mock.patch.object(m, 'ElfImage', return_value=object()), \
             mock.patch.object(m, 'extract_camera_tables', return_value=b'camera'), \
             mock.patch.object(m, 'import_ui_elf', return_value=(b'ui', {})), \
             mock.patch.object(m, 'import_hud_elf', return_value=(b'hud', {})), \
             mock.patch.object(m, 'import_commands_elf', side_effect=ValueError('Unsupported command table')), \
             mock.patch.object(m, 'extract_owned_ruw') as disc_import, \
             tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'bundle'
            with self.assertRaisesRegex(ValueError, 'Unsupported command'):
                artifacts, manifest = m.import_resources(elf, Path('owned-disc'))
                m.write_bundle(output, artifacts, manifest)
            self.assertFalse(output.exists())
            disc_import.assert_not_called()

    def test_camera_rows_preserve_two_indices_three_slots_and_payloads(self):
        image = SyntheticImage()
        initial = bytes((i * 3) & 0xff for i in range(0x90))
        initial_tail = bytes((i * 5 + 7) & 0xff for i in range(0x90))
        later = bytes((255 - i) & 0xff for i in range(0x90))
        image.write(0x2000, initial)
        image.write(0x2100, initial_tail)
        image.write(0x2200, later)
        put_row(image, 17, 0, (0x2000, 0, 0x2100))
        put_row(image, 17, 1, (0x2200, 0, 0))

        digest = bytes(range(32))
        data = m.extract_camera_tables(image, digest)
        self.assertEqual(m.CAMERA_HEADER.unpack_from(data),
                         (b'OKBTCAM1', 1, 23, digest))
        row_bytes = 3 * m.CAMERA_SLOT_SIZE
        stage17 = m.CAMERA_HEADER.size + 16 * 2 * row_bytes
        self.assertEqual(data[stage17:stage17 + m.CAMERA_SLOT_SIZE], struct.pack('<I', 1) + initial)
        self.assertEqual(data[stage17 + m.CAMERA_SLOT_SIZE:
                              stage17 + 2 * m.CAMERA_SLOT_SIZE], bytes(m.CAMERA_SLOT_SIZE))
        self.assertEqual(data[stage17 + 2 * m.CAMERA_SLOT_SIZE:
                              stage17 + 3 * m.CAMERA_SLOT_SIZE], struct.pack('<I', 1) + initial_tail)
        later_start = stage17 + row_bytes
        self.assertEqual(data[later_start:later_start + m.CAMERA_SLOT_SIZE], struct.pack('<I', 1) + later)
        self.assertEqual(data[later_start + m.CAMERA_SLOT_SIZE:
                              later_start + 3 * m.CAMERA_SLOT_SIZE], bytes(2 * m.CAMERA_SLOT_SIZE))
        self.assertEqual(len(data), m.CAMERA_HEADER.size + 23 * 2 * row_bytes)

    def test_camera_rejects_bad_digest_alignment_and_unmapped_payload(self):
        image = SyntheticImage()
        with self.assertRaisesRegex(ValueError, 'digest'):
            m.extract_camera_tables(image, b'short')
        put_row(image, 1, 0, (0x2001, 0, 0))
        with self.assertRaisesRegex(ValueError, 'Unaligned'):
            m.extract_camera_tables(image, bytes(32))
        put_row(image, 1, 0, (0x500000, 0, 0))
        with self.assertRaisesRegex(ValueError, 'Unmapped'):
            m.extract_camera_tables(image, bytes(32))

    def test_writer_creates_only_fresh_output_and_rejects_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            target = root / 'bundle'
            m.write_bundle(target, {'asset.bin': b'abc'}, {'test': True})
            self.assertEqual((target / 'asset.bin').read_bytes(), b'abc')
            self.assertIn('true', (target / 'manifest.json').read_text())
            with self.assertRaisesRegex(ValueError, 'already exists'):
                m.write_bundle(target, {'asset.bin': b'different'}, {})
            link = root / 'alias'
            link.symlink_to(target, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, 'symlink'):
                m.write_bundle(link, {'asset.bin': b'abc'}, {})

    def test_writer_refuses_unsafe_artifact_name_without_leaving_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / 'bundle'
            with self.assertRaisesRegex(ValueError, 'Unsafe artifact'):
                m.write_bundle(target, {'../escape': b'x'}, {})
            self.assertFalse(target.exists())

    def test_cli_rejects_non_owned_elf_without_creating_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            elf = root / 'fake.elf'
            disc = root / 'fake.bin'
            output = root / 'bundle'
            elf.write_bytes(b'not the owned ELF')
            disc.write_bytes(b'not the owned disc')
            result = subprocess.run(
                [sys.executable, str(ROOT / 'tools/import_battle_resources.py'),
                 '--elf', str(elf), '--disc', str(disc), '--output-dir', str(output)],
                capture_output=True, text=True, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())

    def test_ruw_import_rejects_unowned_disc_before_parsing(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / 'wrong-disc.bin'
            image.write_bytes(b'not the pinned owned image')
            with self.assertRaisesRegex(ValueError, 'Disc SHA256'):
                m.extract_owned_ruw(image)

if __name__ == '__main__':
    unittest.main()
