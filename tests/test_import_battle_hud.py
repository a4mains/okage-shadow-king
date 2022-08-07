import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import import_battle_hud as hud
SPEC = importlib.util.spec_from_file_location('import_battle_resources_hud_test', ROOT / 'tools/import_battle_resources.py')
bundle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bundle)

class SyntheticImage:
    def __init__(self, *, palette=None, threshold=0.1, strings=None):
        self.palette = palette if palette is not None else struct.pack('<18i', *range(-9, 9))
        self.threshold = struct.pack('<d', threshold)
        if strings is None:
            strings = (b'\\f[1]\\c[%d]00\\c[0]0', b'%03d', b'\\f[1]\\c[%d]%s\\c[0]%s',
                       b'\\f[1]\\c[%d]0\\c[0]0', b'%02d')
        self.ranges = {
            hud.PALETTE_ADDRESS: self.palette,
            hud.THRESHOLD_ADDRESS: self.threshold,
        }
        for address, value in zip(hud.STRING_ADDRESSES, strings):
            self.ranges[address] = (value + b'\0').ljust(hud.STRING_BYTES, b'\0')[:hud.STRING_BYTES]

    def read(self, address, size):
        value = self.ranges.get(address)
        if value is None or size > len(value):
            raise ValueError(f'Unmapped synthetic range {address:#x}+{size:#x}')
        return value[:size]

class BattleHudImporterTests(unittest.TestCase):
    def test_fixed_header_payload_order_and_metadata(self):
        image = SyntheticImage()
        digest = bytes(range(32))
        payload, metadata = hud.extract_payload(image, digest)
        self.assertEqual(len(payload), hud.PAYLOAD_BYTES)
        self.assertEqual(payload[:72], image.palette)
        self.assertEqual(struct.unpack_from('<d', payload, 72)[0], 0.1)
        strings_start = 80
        for index, address in enumerate(hud.STRING_ADDRESSES):
            raw = image.ranges[address]
            nul = raw.index(0)
            slot = payload[strings_start + index * 64:strings_start + (index + 1) * 64]
            self.assertEqual(slot, raw[:nul] + bytes(64 - nul))
        fake_elf = b'fake elf fixture for importer wrapper'
        with mock.patch.object(hud, 'ELF_SHA256', hashlib.sha256(fake_elf).hexdigest()), \
             mock.patch.object(hud, 'ElfImage', return_value=image):
            artifact, artifact_meta = hud.import_elf(fake_elf)
        magic, version, payload_size, source_digest, payload_digest = hud.HEADER.unpack_from(artifact)
        self.assertEqual((magic, version, payload_size), (hud.MAGIC, 2, 400))
        self.assertEqual(source_digest.hex(), hashlib.sha256(fake_elf).hexdigest())
        self.assertEqual(payload_digest, hashlib.sha256(payload).digest())
        self.assertEqual(artifact[hud.HEADER.size:], payload)
        self.assertEqual(len(artifact), 480)
        self.assertEqual(artifact_meta['artifact_sha256'], hashlib.sha256(artifact).hexdigest())
        self.assertEqual(metadata['elf_sha256'], digest.hex())
        self.assertEqual(metadata['sources']['palette']['signed_words'], list(range(-9, 9)))
        self.assertEqual([row['vaddr'] for row in metadata['sources']['format_strings']],
                         [f'0x{address:08x}' for address in hud.STRING_ADDRESSES])
        self.assertEqual(metadata['source_provenance']['formatting_function'], '0x001240c8')
        self.assertEqual(metadata['source_provenance']['resource_formatting_function'], '0x00124200')

    def test_unterminated_empty_non_ascii_strings_and_bad_threshold_rejected(self):
        strings = [b'a', b'b', b'c', b'd', b'e']
        strings[1] = b'x' * hud.STRING_BYTES
        with self.assertRaisesRegex(ValueError, 'Unterminated'):
            hud.extract_payload(SyntheticImage(strings=strings), bytes(32))
        strings[1] = b''
        with self.assertRaisesRegex(ValueError, 'Empty'):
            hud.extract_payload(SyntheticImage(strings=strings), bytes(32))
        strings[1] = b'\xff'
        with self.assertRaisesRegex(ValueError, 'Non-ASCII'):
            hud.extract_payload(SyntheticImage(strings=strings), bytes(32))
        strings[1] = b'b'
        strings[4] = b'x' * hud.STRING_BYTES
        with self.assertRaisesRegex(ValueError, 'Unterminated'):
            hud.extract_payload(SyntheticImage(strings=strings), bytes(32))
        with self.assertRaisesRegex(ValueError, 'threshold'):
            hud.extract_payload(SyntheticImage(threshold=0.2), bytes(32))

    def test_source_digest_size_and_owned_elf_hash_guards(self):
        with self.assertRaisesRegex(ValueError, 'digest'):
            hud.extract_payload(SyntheticImage(), b'short')
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            hud.import_elf(b'not the pinned retail ELF')

    def test_output_header_sha_and_manifest_are_consistent(self):
        payload = bytes(hud.PAYLOAD_BYTES)
        digest = bytes(range(32))
        header = hud.HEADER.pack(hud.MAGIC, hud.VERSION, hud.PAYLOAD_BYTES,
                                 digest, hashlib.sha256(payload).digest())
        self.assertEqual(len(header + payload), 480)
        self.assertEqual(hud.HEADER.unpack(header),
                         (b'OKBHUD2\0', 2, 400, digest, hashlib.sha256(payload).digest()))

    def test_output_writer_refuses_overwrite_and_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output, manifest = root / 'battle-hud.bin', root / 'battle-hud.json'
            hud.write_outputs(output, manifest, b'asset', {'ok': True})
            self.assertEqual(output.read_bytes(), b'asset')
            self.assertTrue(json.loads(manifest.read_text())['ok'])
            with self.assertRaisesRegex(ValueError, 'existing'):
                hud.write_outputs(output, root / 'other.json', b'new', {})
            link = root / 'link.bin'
            link.symlink_to(output)
            with self.assertRaisesRegex(ValueError, 'symlink'):
                hud.write_outputs(link, root / 'link.json', b'x', {})

    def test_bundle_importer_adds_hud_artifact_and_metadata(self):
        elf = b'fake pinned ELF bytes'
        digest = hashlib.sha256(elf).hexdigest()
        hud_meta = {'schema': 'okage-battle-hud-import-v1', 'artifact_sha256': 'a' * 64}
        ui_meta = {'schema': 'okage-battle-ui-import-v1'}
        entry = {'path': 'CMNDATA/WIN/BTL.RUW', 'lba': 42, 'size': 19596}
        with mock.patch.object(bundle, 'ELF_SHA256', digest), \
             mock.patch.object(bundle, 'import_elf', return_value=b'data'), \
             mock.patch.object(bundle, 'ElfImage'), \
             mock.patch.object(bundle, 'extract_camera_tables', return_value=b'camera'), \
             mock.patch.object(bundle, 'import_ui_elf', return_value=(b'ui', ui_meta)), \
             mock.patch.object(bundle, 'import_hud_elf', return_value=(b'hud', hud_meta)), \
             mock.patch.object(bundle, 'extract_owned_ruw', return_value=(b'ruw', entry)):
            artifacts, manifest = bundle.import_resources(elf, Path('not-read-by-mock'))
        self.assertEqual(set(artifacts), {'battle-data.bin', 'BTL.RUW', 'battle-camera.bin',
                                          'battle-ui.bin', 'battle-hud.bin'})
        self.assertEqual(artifacts['battle-hud.bin'], b'hud')
        self.assertEqual(manifest['hud_format'], hud_meta)
        self.assertEqual(manifest['artifacts']['battle-hud.bin']['size'], 3)

    def test_cli_rejects_unowned_elf_without_output_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            elf = root / 'wrong.elf'
            output = root / 'hud.bin'
            elf.write_bytes(b'not the pinned ELF')
            result = subprocess.run([sys.executable, str(ROOT / 'tools/import_battle_hud.py'),
                                     '--elf', str(elf), '--output', str(output)],
                                    capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertFalse(output.exists())
            self.assertFalse(output.with_name(output.name + '.manifest.json').exists())

if __name__ == '__main__':
    unittest.main()
