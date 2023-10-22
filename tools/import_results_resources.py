#!/usr/bin/env python3
"""Extend a verified first-battle bundle with owned results resources.

The output is a fresh directory containing the unchanged battle bundle, the
original bundle manifest as ``battle-manifest.json``, results-ui.bin,
results-text.bin, FLD.RUW, and a manifest that binds every output file and
records each source path and SHA256.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

from import_battle_resources import sha256, write_bundle
from import_battle_data import ELF_SHA256
from import_results_ui import import_elf as import_results_ui_elf
from import_results_text import import_elf as import_results_text_elf

FIELD_RUW_SHA256 = 'e66a179416ae73d213dbc2d634a1486fffe57ce3438c79794cf156f2f5dd00c8'
OUTPUT_NAMES = {'manifest.json', 'battle-manifest.json', 'results-ui.bin',
                'results-text.bin', 'FLD.RUW'}
EXTRA_MANIFESTS = {
    'battle-actions.bin': 'battle-actions.bin.manifest.json',
    'battle-enemy-commands.bin': 'battle-enemy-commands.bin.manifest.json',
}

def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()

def _ordinary_file(path, label):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'{label} must be an ordinary file, not a symlink: {path}')
    return path

def _load_json(path, label):
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f'Invalid {label}: {exc}') from exc

def _manifest_artifact(record, name):
    if not isinstance(record, dict):
        raise ValueError(f'Invalid artifact record for {name}')
    digest = record.get('sha256') or record.get('artifact_sha256')
    size = record.get('size', record.get('artifact_bytes'))
    if not isinstance(digest, str) or len(digest) != 64:
        raise ValueError(f'Missing SHA256 for {name} manifest')
    if not isinstance(size, int) or isinstance(size, bool) or size < 0:
        raise ValueError(f'Missing size for {name} manifest')
    return size, digest.lower()

def _validate_artifact(path, record, name):
    path = _ordinary_file(path, f'Battle artifact {name}')
    expected_size, expected_hash = _manifest_artifact(record, name)
    actual_size = path.stat().st_size
    actual_hash = sha256_file(path)
    if actual_size != expected_size or actual_hash != expected_hash:
        raise ValueError(f'Battle artifact hash or size mismatch: {name}')
    return path.read_bytes()

def _read_battle_bundle(directory):
    root = Path(directory)
    if root.is_symlink() or not root.is_dir():
        raise ValueError('Battle input must be an ordinary directory, not a symlink')
    manifest_path = _ordinary_file(root / 'manifest.json', 'Battle manifest')
    manifest = _load_json(manifest_path, 'battle manifest')
    records = manifest.get('artifacts') if isinstance(manifest, dict) else None
    if not isinstance(records, dict) or not records:
        raise ValueError('Battle manifest has no artifact hash table')

    artifacts = {}
    for name, record in records.items():
        if not isinstance(name, str) or Path(name).name != name or name in ('', '.', '..'):
            raise ValueError('Unsafe artifact name in battle manifest')
        artifacts[name] = _validate_artifact(root / name, record, name)

    for artifact_name, sidecar_name in EXTRA_MANIFESTS.items():
        artifact_path = root / artifact_name
        sidecar_path = root / sidecar_name
        if artifact_path.exists() or artifact_path.is_symlink():
            if artifact_name in records:
                continue
            sidecar = _ordinary_file(sidecar_path, f'{artifact_name} manifest')
            sidecar_data = _load_json(sidecar, f'{artifact_name} manifest')
            if not isinstance(sidecar_data, dict):
                raise ValueError(f'Invalid {artifact_name} manifest')
            artifacts[artifact_name] = _validate_artifact(artifact_path, sidecar_data,
                                                          artifact_name)
            artifacts[sidecar_name] = sidecar.read_bytes()
        elif sidecar_path.exists() or sidecar_path.is_symlink():
            raise ValueError(f'{artifact_name} manifest has no artifact')

    for child in root.iterdir():
        if child.is_symlink():
            raise ValueError(f'Refusing symlink in battle input: {child.name}')
        if child.is_dir():
            raise ValueError(f'Unexpected directory in battle input: {child.name}')
        if not child.is_file():
            raise ValueError(f'Unexpected non-file in battle input: {child.name}')
        name = child.name
        if name == 'manifest.json':
            continue
        if name in OUTPUT_NAMES:
            raise ValueError(f'Battle input uses reserved output name: {name}')
        if name not in artifacts:
            artifacts[name] = child.read_bytes()

    artifacts['battle-manifest.json'] = manifest_path.read_bytes()
    source_manifest_hash = sha256(artifacts['battle-manifest.json'])
    provenance = {
        name: {'source_path': str((root / name).absolute()),
               'sha256': sha256(data), 'size': len(data)}
        for name, data in artifacts.items()
    }
    provenance['manifest.json'] = {
        'source_path': str(manifest_path.absolute()),
        'sha256': source_manifest_hash,
        'size': len(artifacts['battle-manifest.json']),
    }
    return artifacts, provenance

def import_resources(elf_data, field_ruw_path, battle_input_dir, elf_source_path='<provided ELF bytes>'):
    """Validate and extend a battle bundle with results artifacts."""
    digest = sha256(elf_data)
    if digest != ELF_SHA256:
        raise ValueError('Owned ELF SHA256 does not match supported retail revision')
    field_path = _ordinary_file(field_ruw_path, 'FLD.RUW input')
    field_data = field_path.read_bytes()
    if sha256(field_data) != FIELD_RUW_SHA256:
        raise ValueError('FLD.RUW SHA256 does not match supported owned resource')

    artifacts, sources = _read_battle_bundle(battle_input_dir)
    results_ui, ui_manifest = import_results_ui_elf(elf_data)
    results_text, text_manifest = import_results_text_elf(elf_data)
    artifacts.update({'results-ui.bin': results_ui, 'results-text.bin': results_text,
                      'FLD.RUW': field_data})
    sources['elf'] = {'source_path': str(Path(elf_source_path).absolute()) if elf_source_path != '<provided ELF bytes>' else elf_source_path, 'sha256': digest,
                      'size': len(elf_data)}
    sources['FLD.RUW'] = {'source_path': str(field_path.absolute()),
                           'sha256': sha256(field_data), 'size': len(field_data)}
    output_manifest = {
        'format': 'okage-owned-results-resources-v1',
        'sources': sources,
        'artifacts': {name: {'size': len(data), 'sha256': sha256(data)}
                      for name, data in artifacts.items()},
        'results_ui_format': ui_manifest,
        'results_text_format': text_manifest,
    }
    return artifacts, output_manifest

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True,
                        help='Hash-pinned owned ELF32 file')
    parser.add_argument('--field-ruw', type=Path, required=True,
                        help='Hash-pinned loose FLD.RUW file')
    parser.add_argument('--battle-input', type=Path, required=True,
                        help='Existing verified battle-resource bundle directory')
    parser.add_argument('--output-dir', type=Path, required=True,
                        help='New output directory')
    args = parser.parse_args(argv)
    try:
        elf_path = _ordinary_file(args.elf, 'ELF input')
        artifacts, manifest = import_resources(elf_path.read_bytes(), args.field_ruw,
                                               args.battle_input, elf_path)
        write_bundle(args.output_dir, artifacts, manifest)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f'results resource import failed: {exc}', file=sys.stderr)
        return 2
    print(f'Imported {len(artifacts)} assets to {args.output_dir}')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
