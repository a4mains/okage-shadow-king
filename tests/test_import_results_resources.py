import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
SPEC = importlib.util.spec_from_file_location(
    'import_results_resources', ROOT / 'tools/import_results_resources.py')
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)

def sha(data):
    return hashlib.sha256(data).hexdigest()

class ResultsResourcesTests(unittest.TestCase):
    def make_battle_bundle(self, root):
        root.mkdir()
        data = {'battle-data.bin': b'battle', 'BTL.RUW': b'btlruw'}
        records = {name: {'size': len(value), 'sha256': sha(value)}
                   for name, value in data.items()}
        (root / 'manifest.json').write_text(json.dumps({'artifacts': records}))
        for name, value in data.items():
            (root / name).write_bytes(value)
        actions = b'actions'
        sidecar = {'artifact_bytes': len(actions), 'artifact_sha256': sha(actions),
                   'schema': 'okage-battle-action-data-v1'}
        (root / 'battle-actions.bin').write_bytes(actions)
        (root / 'battle-actions.bin.manifest.json').write_text(json.dumps(sidecar))
        enemy = b'enemy'
        enemy_sidecar = {'artifact_bytes': len(enemy), 'artifact_sha256': sha(enemy),
                         'schema': 'okage-battle-enemy-commands-v1'}
        (root / 'battle-enemy-commands.bin').write_bytes(enemy)
        (root / 'battle-enemy-commands.bin.manifest.json').write_text(json.dumps(enemy_sidecar))
        return data

    def prepare_patches(self, elf, field):
        return (mock.patch.object(m, 'ELF_SHA256', sha(elf)),
                mock.patch.object(m, 'FIELD_RUW_SHA256', sha(field)),
                mock.patch.object(m, 'import_results_ui_elf', return_value=(b'results-ui', {'schema': 'ui'})),
                mock.patch.object(m, 'import_results_text_elf', return_value=(b'results-text', {'schema': 'text'})))

    def test_import_extends_verified_bundle_and_preserves_source_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bundle = root / 'battle'
            original = self.make_battle_bundle(bundle)
            source_manifest = (bundle / 'manifest.json').read_bytes()
            field = root / 'FLD.RUW'
            field.write_bytes(b'field-data')
            elf = b'owned-elf'
            patches = self.prepare_patches(elf, b'field-data')
            with patches[0], patches[1], patches[2], patches[3]:
                artifacts, manifest = m.import_resources(elf, field, bundle, root / 'game.elf')
            self.assertEqual(artifacts['battle-data.bin'], original['battle-data.bin'])
            self.assertEqual(artifacts['battle-actions.bin'], b'actions')
            self.assertEqual(artifacts['results-ui.bin'], b'results-ui')
            self.assertEqual(artifacts['results-text.bin'], b'results-text')
            self.assertEqual(artifacts['FLD.RUW'], b'field-data')
            self.assertEqual(artifacts['battle-manifest.json'], source_manifest)
            self.assertEqual(manifest['sources']['elf']['source_path'], str((root / 'game.elf').absolute()))
            for name, data in artifacts.items():
                self.assertEqual(manifest['artifacts'][name], {'size': len(data), 'sha256': sha(data)})
            output = root / 'combined'
            m.write_bundle(output, artifacts, manifest)
            self.assertEqual((output / 'manifest.json').exists(), True)
            self.assertEqual((output / 'battle-manifest.json').read_bytes(), source_manifest)
            self.assertEqual((output / 'battle-actions.bin.manifest.json').read_bytes(),
                             (bundle / 'battle-actions.bin.manifest.json').read_bytes())

    def test_rejects_invalid_elf_field_hash_and_inherited_artifact_tamper(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bundle = root / 'battle'
            self.make_battle_bundle(bundle)
            field = root / 'FLD.RUW'
            field.write_bytes(b'field-data')
            with mock.patch.object(m, 'ELF_SHA256', '0' * 64):
                with self.assertRaisesRegex(ValueError, 'ELF SHA256'):
                    m.import_resources(b'wrong', field, bundle)
            elf = b'owned-elf'
            with mock.patch.object(m, 'ELF_SHA256', sha(elf)), \
                    mock.patch.object(m, 'FIELD_RUW_SHA256', '0' * 64):
                with self.assertRaisesRegex(ValueError, 'FLD.RUW SHA256'):
                    m.import_resources(elf, field, bundle)
            (bundle / 'battle-data.bin').write_bytes(b'tampered')
            with mock.patch.object(m, 'ELF_SHA256', sha(elf)), \
                    mock.patch.object(m, 'FIELD_RUW_SHA256', sha(b'field-data')):
                with self.assertRaisesRegex(ValueError, 'hash or size mismatch'):
                    m.import_resources(elf, field, bundle)

    def test_rejects_tampered_sidecar_artifact_not_in_main_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bundle = root / 'battle'
            self.make_battle_bundle(bundle)
            field = root / 'FLD.RUW'
            field.write_bytes(b'field-data')
            (bundle / 'battle-actions.bin').write_bytes(b'edited-action-data')
            elf = b'owned-elf'
            with mock.patch.object(m, 'ELF_SHA256', sha(elf)), \
                    mock.patch.object(m, 'FIELD_RUW_SHA256', sha(b'field-data')):
                with self.assertRaisesRegex(ValueError, 'hash or size mismatch'):
                    m.import_resources(elf, field, bundle)

    def test_rejects_symlinks_and_fresh_output_reuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bundle = root / 'battle'
            self.make_battle_bundle(bundle)
            field = root / 'FLD.RUW'
            field.write_bytes(b'field-data')
            elf = b'owned-elf'
            (bundle / 'battle-data.bin').unlink()
            (bundle / 'battle-data.bin').symlink_to(root / 'outside')
            with mock.patch.object(m, 'ELF_SHA256', sha(elf)), \
                    mock.patch.object(m, 'FIELD_RUW_SHA256', sha(b'field-data')):
                with self.assertRaisesRegex(ValueError, 'symlink'):
                    m.import_resources(elf, field, bundle)
            with tempfile.TemporaryDirectory() as target_tmp:
                target = Path(target_tmp) / 'existing'
                target.mkdir()
                with self.assertRaisesRegex(ValueError, 'already exists'):
                    m.write_bundle(target, {'new.bin': b'x'}, {})
                self.assertEqual(list(target.iterdir()), [])

if __name__ == '__main__':
    unittest.main()
