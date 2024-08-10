import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
import import_sound_set as mod

class FakeDisc:
    def __init__(self, _path, payloads):
        self.payloads = payloads
        self.closed = False

    def read(self, lba, size):
        payload = self.payloads[lba]
        if len(payload) != size:
            raise ValueError('unexpected synthetic extent size')
        return payload

    def close(self):
        self.closed = True

class ImportSoundSetTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=Path.cwd())
        self.root = Path(self.tmp.name)
        self.report = self.root / 'disc-report.json'
        self.asset_root = self.root / 'audio-input'
        self.output = self.root / 'sets.json'
        self.payloads = {101: b'first synthetic RXWS', 202: b'second synthetic RXWS'}
        self.entries = {'files': [
            {'path': 'SNDDATA/SE/SET1.XWS', 'lba': 101, 'size': len(self.payloads[101])},
            {'path': 'SNDDATA/SE/SET2.XWS', 'lba': 202, 'size': len(self.payloads[202])},
            {'path': 'SNDDATA/SE/ALL.XWS', 'lba': 303, 'size': 1},
            {'path': 'SNDDATA/SE/3XVFILE.XV', 'lba': 404, 'size': 1},
        ]}
        self.report.write_text(json.dumps(self.entries))
        self.pins = {i: hashlib.sha256(data).hexdigest()
                     for i, data in zip((1, 2), self.payloads.values())}
        self.hash_patch = patch.dict(mod.HASHES, self.pins, clear=True)
        self.hash_patch.start()
        self.disc_patch = patch.object(mod, 'Disc', side_effect=lambda path: FakeDisc(path, self.payloads))
        self.disc_patch.start()

    def tearDown(self):
        self.disc_patch.stop()
        self.hash_patch.stop()
        self.tmp.cleanup()

    def run_bulk(self):
        return mod.import_sets((1, 2), self.output, all_sets=True,
                               entries_path=self.report, disc_path=self.root / 'disc.bin',
                               asset_root=self.asset_root)

    def test_bulk_success_and_identical_reuse(self):
        records = self.run_bulk()
        self.assertEqual([r['path'] for r in records], [
            'SNDDATA/SE/SET1.XWS', 'SNDDATA/SE/SET2.XWS'])
        for record, payload in zip(records, self.payloads.values()):
            self.assertEqual((self.root / record['destination']).read_bytes(), payload)
        manifest = json.loads(self.output.read_text())
        self.assertEqual(manifest['schema'], mod.INVENTORY_SCHEMA)
        self.assertEqual(manifest['sets'], records)
        self.run_bulk()
        self.assertEqual(json.loads(self.output.read_text()), manifest)

    def test_changed_selected_hash_leaves_no_outputs(self):
        bad_payloads = dict(self.payloads)
        bad_payloads[202] = b'X' + bad_payloads[202][1:]
        self.disc_patch.stop()
        self.disc_patch = patch.object(mod, 'Disc', side_effect=lambda path: FakeDisc(path, bad_payloads))
        self.disc_patch.start()
        with self.assertRaisesRegex(ValueError, 'Different owned sound-set'):
            self.run_bulk()
        self.assertFalse(self.asset_root.exists())
        self.assertFalse(self.output.exists())

    def test_later_asset_collision_prevents_any_new_asset_or_manifest(self):
        second = self.asset_root / 'SNDDATA/SE/SET2.XWS'
        second.parent.mkdir(parents=True)
        second.write_bytes(b'user-edited bytes')
        with self.assertRaisesRegex(RuntimeError, 'Refusing changed output'):
            self.run_bulk()
        self.assertFalse((second.parent / 'SET1.XWS').exists())
        self.assertFalse(self.output.exists())
        self.assertEqual(second.read_bytes(), b'user-edited bytes')

    def test_changed_manifest_prevents_any_asset_write(self):
        self.output.write_bytes(b'user-edited manifest')
        with self.assertRaisesRegex(RuntimeError, 'Refusing changed output'):
            self.run_bulk()
        self.assertFalse(self.asset_root.exists())
        self.assertEqual(self.output.read_bytes(), b'user-edited manifest')

    def test_normalized_manifest_asset_alias_is_rejected_before_writes(self):
        alias = self.asset_root / 'SNDDATA/SE/../SE/SET1.XWS'
        with self.assertRaisesRegex(RuntimeError, 'duplicate output path'):
            mod.import_sets((1, 2), alias, all_sets=True,
                            entries_path=self.report, disc_path=self.root / 'disc.bin',
                            asset_root=self.asset_root)
        self.assertFalse(self.asset_root.exists())

    def test_output_ancestor_of_asset_is_rejected_before_writes(self):
        with self.assertRaisesRegex(RuntimeError, 'ancestor conflict'):
            mod.import_sets((1, 2), self.asset_root, all_sets=True,
                            entries_path=self.report, disc_path=self.root / 'disc.bin',
                            asset_root=self.asset_root)
        self.assertFalse(self.asset_root.exists())

    def test_symlink_manifest_output_prevents_any_asset_write(self):
        target = self.root / 'manifest-target.json'
        target.write_bytes(b'keep target unchanged')
        self.output.symlink_to(target)
        with self.assertRaisesRegex(RuntimeError, 'symlink output path component'):
            self.run_bulk()
        self.assertFalse(self.asset_root.exists())
        self.assertEqual(target.read_bytes(), b'keep target unchanged')

    def test_symlink_asset_ancestor_prevents_any_manifest_or_asset_write(self):
        target = self.root / 'redirect-target'
        target.mkdir()
        link = self.root / 'redirected-assets'
        link.symlink_to(target, target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, 'symlink output path component'):
            mod.import_sets((1, 2), self.output, all_sets=True,
                            entries_path=self.report, disc_path=self.root / 'disc.bin',
                            asset_root=link)
        self.assertEqual(list(target.iterdir()), [])
        self.assertFalse(self.output.exists())

    def test_single_set_manifest_shape_remains_compatible(self):
        records = mod.import_sets((1,), self.output, entries_path=self.report,
                                  disc_path=self.root / 'disc.bin', asset_root=self.asset_root)
        self.assertEqual(records[0], json.loads(self.output.read_text()))
        self.assertNotIn('schema', json.loads(self.output.read_text()))

    def test_set_and_all_arguments_are_mutually_exclusive(self):
        with self.assertRaises(SystemExit) as error:
            mod.main(['--all', '--set', '1', '--output', str(self.output)])
        self.assertEqual(error.exception.code, 2)

if __name__ == '__main__':
    unittest.main()
