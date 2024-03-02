"""Focused provenance and fail-closed tests for title resource imports."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

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

    def test_gain_table_uses_elf_load_mapping_and_records_source_provenance(self):
        values = [0] * 257
        values[255], values[256], values[0], values[1] = 4096, 4096, -4096, -4032
        table = struct.pack('<257h', *values)
        file_offset = 176 + resources.IOP_GAIN_TABLE_VADDR
        elf = bytearray(file_offset + len(table))
        elf[file_offset:file_offset + len(table)] = table
        segment = {'type': 1, 'offset': 176, 'vaddr': '0x00000000',
                   'filesz': len(elf) - 176, 'memsz': len(elf) - 176}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'RXSNDDRV.IRX'
            path.write_bytes(elf)
            with patch('inspect_disc.inspect_elf', return_value={'segments': [segment]}), \
                 patch.object(resources, 'IOP_IRX_SHA256', hashlib.sha256(elf).hexdigest()), \
                 patch.object(resources, 'IOP_GAIN_TABLE_SHA256', hashlib.sha256(table).hexdigest()):
                provenance, actual = resources.verify_iop_gain_table(path)
        self.assertEqual(actual, table)
        self.assertEqual(provenance['table_file_offset'], 0xd1d0)
        self.assertEqual(provenance['table_vaddr'], '0x0000d120')
        self.assertEqual(provenance['selected_pt_load']['offset'], 176)
        self.assertEqual(provenance['table_bytes'], 514)
        self.assertEqual(provenance['table_sha256'], hashlib.sha256(table).hexdigest())
        self.assertEqual(provenance['examples'], {'127': 4096, '128': 4096,
                                                   '-128': -4096, '-127': -4032})

    def test_gain_table_rejects_wrong_module_hash_and_non_file_backed_extent(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'RXSNDDRV.IRX'
            path.write_bytes(b'not the pinned module')
            with self.assertRaisesRegex(ValueError, 'hash differs'):
                resources.verify_iop_gain_table(path)

            path.write_bytes(b'fixture')
            segment = {'type': 1, 'offset': 0, 'vaddr': '0x00000000', 'filesz': 1, 'memsz': 1}
            with patch('inspect_disc.inspect_elf', return_value={'segments': [segment]}), \
                 patch.object(resources, 'IOP_IRX_SHA256', hashlib.sha256(b'fixture').hexdigest()):
                with self.assertRaisesRegex(ValueError, 'outside file-backed'):
                    resources.verify_iop_gain_table(path)

            table_end = 176 + resources.IOP_GAIN_TABLE_VADDR + resources.IOP_GAIN_TABLE_BYTES
            bounded = bytearray(table_end)
            path.write_bytes(bounded)
            bounded_segment = {'type': 1, 'offset': 176, 'vaddr': '0x00000000',
                               'filesz': len(bounded) - 176, 'memsz': len(bounded) - 176}
            with patch('inspect_disc.inspect_elf', return_value={'segments': [bounded_segment]}), \
                 patch.object(resources, 'IOP_IRX_SHA256', hashlib.sha256(bounded).hexdigest()):
                with self.assertRaisesRegex(ValueError, 'table hash differs'):
                    resources.verify_iop_gain_table(path)

    def test_gain_table_install_conflict_preflights_streams(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            conflict = root / resources.IOP_GAIN_TABLE_FILENAME
            conflict.write_bytes(b'changed gain table')
            payloads = [('SNDDATA/STR/THEMA_1.XWH', b'new header'),
                        ('SNDDATA/STR/THEMA_1.XWB', b'new bank'),
                        (resources.IOP_GAIN_TABLE_FILENAME, b'verified table')]
            with self.assertRaisesRegex(RuntimeError, 'Refusing changed output'):
                resources.install_audio_resources(root, payloads)
            self.assertFalse((root / 'THEMA_1.XWH').exists())
            self.assertFalse((root / 'THEMA_1.XWB').exists())
            self.assertEqual(conflict.read_bytes(), b'changed gain table')

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
