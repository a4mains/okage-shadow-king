"""Focused provenance and fail-closed tests for title resource imports."""
import hashlib
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location('import_title_resources', TOOLS / 'import_title_resources.py')
resources = importlib.util.module_from_spec(spec)
spec.loader.exec_module(resources)

class FakeDisc:
    def __init__(self, payloads):
        self.payloads = payloads
        self.reads = []
        self.closed = False

    def read(self, lba, size):
        self.reads.append((lba, size))
        return self.payloads[(lba, size)]

    def close(self):
        self.closed = True

class TitleResourceImportTests(unittest.TestCase):
    def test_pinned_title_strings_are_exact_and_file_backed(self):
        addresses = list(resources.ELF_STRING_ADDRESSES)
        segments = [{'type': 1, 'vaddr': f'0x{addresses[0]:08x}', 'filesz': 1024, 'offset': 0}]
        elf = bytearray(1024)
        for addr, expected in resources.ELF_STRING_ADDRESSES.items():
            start = addr - addresses[0]
            elf[start:start + len(expected) + 1] = expected + b'\0'
        actual = resources.verify_title_strings(bytes(elf), segments)
        self.assertEqual(actual['0x227958'], 'snddata/str/thema_1')
        self.assertEqual(actual['0x227cd8'], '%s.xwh')
        self.assertEqual(actual['0x227ce0'], '%s.xwb')
        with self.assertRaisesRegex(ValueError, 'outside file-backed'):
            resources.address_bytes(bytes(elf), segments, addresses[-1] + 500, 1)

    def test_xpf_provenance_requires_pinned_inputs_and_unique_members(self):
        report = {'elf_sha256': resources.ELF_HASH, 'image_sha256': resources.DISC_SHA256,
                  'all_corpus': True, 'results': [{'archive': 'A', 'member': 'b', 'sha256': '00'}]}
        self.assertIn(('A', 'b'), resources.index_xpf_members(report))
        duplicate = dict(report, results=report['results'] * 2)
        with self.assertRaisesRegex(ValueError, 'Duplicate XPF member'):
            resources.index_xpf_members(duplicate)
        with self.assertRaisesRegex(ValueError, 'pinned USA'):
            resources.index_xpf_members(dict(report, image_sha256='wrong'))

    def test_decoded_asset_must_match_verified_member_and_bytes(self):
        payload = b'title asset fixture'
        expected_hash = hashlib.sha256(payload).hexdigest()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            target = root / 'CMNDATA/2D/SCR.XPF/fixture.xim'
            target.parent.mkdir(parents=True)
            target.write_bytes(payload)
            verification = {'elf_sha256': resources.ELF_HASH, 'image_sha256': resources.DISC_SHA256,
                            'all_corpus': True, 'native_binary_sha256': 'test', 'archives': 1, 'members': 1,
                            'results': [{'archive': 'CMNDATA/2D/SCR.XPF', 'member': 'fixture.xim',
                                         'size': len(payload), 'sha256': expected_hash,
                                         'packed_size': 7, 'consumed': 7}]}
            old_assets = resources.TITLE_ASSETS
            resources.TITLE_ASSETS = (('SCR.XPF', 'fixture.xim', len(payload), expected_hash),)
            try:
                rows = resources.verify_decoded_assets(root, verification)
                self.assertEqual(rows[0]['sha256'], expected_hash)
                target.write_bytes(payload + b'!')
                with self.assertRaisesRegex(ValueError, 'differs from verified member'):
                    resources.verify_decoded_assets(root, verification)
                target.unlink()
                target.symlink_to(root / 'missing-target')
                with self.assertRaisesRegex(ValueError, 'unsafe decoded'):
                    resources.verify_decoded_assets(root, verification)
            finally:
                resources.TITLE_ASSETS = old_assets

    def test_audio_install_preflights_all_outputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            conflict = root / 'THEMA_1.XWB'
            conflict.write_bytes(b'old')
            payloads = [('SNDDATA/STR/THEMA_1.XWH', b'new header'),
                        ('SNDDATA/STR/THEMA_1.XWB', b'new bank')]
            with self.assertRaisesRegex(RuntimeError, 'Refusing changed output'):
                resources.install_audio_resources(root, payloads)
            self.assertFalse((root / 'THEMA_1.XWH').exists())
            self.assertEqual(conflict.read_bytes(), b'old')
            conflict.unlink()
            symlink = root / 'THEMA_1.XWH'
            symlink.symlink_to(root / 'missing')
            with self.assertRaisesRegex(RuntimeError, 'symlink output'):
                resources.install_audio_resources(root, payloads)

    def test_disc_index_rejects_duplicate_paths(self):
        with self.assertRaisesRegex(ValueError, 'duplicate disc path'):
            resources.index_disc_files({'files': [{'path': 'A'}, {'path': 'A'}]})

    def test_audio_extracts_exact_xwh_header_and_checks_extents(self):
        xwh = b'W' * 112
        xwb = b'B' * 23186288
        disc = FakeDisc({(163585, 112): xwh, (163586, 23186288): xwb})
        entries = {spec['path']: {'lba': spec['lba'], 'size': spec['size']} for spec in resources.AUDIO}
        rows, payloads = resources.read_audio_resources(disc, entries)
        self.assertEqual(disc.reads, [(163585, 112), (163586, 23186288)])
        self.assertEqual(rows[0]['header_hex'], xwh.hex())
        self.assertEqual(rows[0]['sha256'], hashlib.sha256(xwh).hexdigest())
        self.assertEqual(rows[1]['sha256'], hashlib.sha256(xwb).hexdigest())
        self.assertEqual(payloads[1][1], xwb)
        with self.assertRaisesRegex(ValueError, 'disc index differs'):
            resources.read_audio_resources(FakeDisc({}), {})
        short = FakeDisc({(163585, 112): xwh[:-1]})
        with self.assertRaisesRegex(ValueError, 'Truncated'):
            resources.read_audio_resources(short, entries)

if __name__ == '__main__':
    unittest.main()
