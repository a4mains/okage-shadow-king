#!/usr/bin/env python3
"""Stage owned local game-data inputs into the native runtime's root layout.

This creates a development symlink overlay. It does not extract or import a
fresh disc image, and it never copies saves, runtime output, or reference state.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
from typing import Iterable

MANIFEST_NAME = ".owned-game-data-manifest.json"

SOURCE_LAYOUT = (
    ("local/xpf-decoded", "", "decoded_game_archives"),
    ("local/font-input/CMNDATA/FONT", "CMNDATA/FONT", "owned_fonts"),
    ("local/text-input/CMNDATA/XDB", "CMNDATA/XDB", "owned_tables"),
    ("local/text-input/FLDDATA/MES", "FLDDATA/MES", "owned_messages"),
    ("local/effect-input/CMNDATA/2D", "CMNDATA/2D", "owned_effect_data"),
    ("local/audio-input/SNDDATA", "SNDDATA", "owned_audio"),
    ("local/battle-input-v6", "NATIVE_UI", "owned_native_ui"),
    ("local/restore-data-v1", "NATIVE_UI", "owned_restore_ui"),
)

def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

def _source_files(root: Path) -> Iterable[Path]:
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"source tree contains a symlink: {path}")
        if path.is_file():
            yield path

def _inside(path: Path, directory: Path) -> bool:
    try:
        path.relative_to(directory)
        return True
    except ValueError:
        return False

def build_plan(repo_root: Path) -> tuple[dict[str, dict], dict[str, str]]:
    """Return staged relative paths and source-root provenance."""
    repo_root = repo_root.resolve(strict=True)
    planned: dict[str, dict] = {}
    source_roots: dict[str, str] = {}

    for relative_source, prefix, label in SOURCE_LAYOUT:
        source_root = repo_root / relative_source
        if not source_root.is_dir() or source_root.is_symlink():
            raise FileNotFoundError(f"required source directory is missing or not a directory: {source_root}")
        source_roots[label] = relative_source
        for source in _source_files(source_root):
            tail = source.relative_to(source_root)
            target_relative = Path(prefix, tail) if prefix else tail
            target = target_relative.as_posix()
            if target == MANIFEST_NAME or target.startswith("../"):
                raise ValueError(f"invalid staged path: {target}")
            source_relative = source.relative_to(repo_root).as_posix()
            digest = _sha256(source)
            existing = planned.get(target)
            if existing is None:
                planned[target] = {
                    "sha256": digest,
                    "source": source_relative,
                    "source_label": label,
                    "identical_sources": [],
                }
            elif existing["sha256"] != digest:
                raise ValueError(
                    f"conflicting assets target {target}: {existing['source']} "
                    f"({existing['sha256']}) vs {source_relative} ({digest})"
                )
            else:
                existing["identical_sources"].append({
                    "path": source_relative,
                    "label": label,
                })
    return planned, source_roots

def stage_game_data(repo_root: Path, destination: Path) -> Path:
    """Create a fresh game-data root made of symlinks and a provenance manifest."""
    repo_root = repo_root.resolve(strict=True)
    destination = destination.expanduser().absolute()
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(f"destination already exists: {destination}")

    planned, source_roots = build_plan(repo_root)
    resolved_sources = [(repo_root / source).resolve(strict=True) for source in source_roots.values()]
    resolved_destination = destination.resolve(strict=False)
    if any(_inside(resolved_destination, source) or _inside(source, resolved_destination)
           for source in resolved_sources):
        raise ValueError("destination overlaps a source tree")

    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{destination.name}.stage-", dir=destination.parent))
    try:
        for relative, record in sorted(planned.items()):
            source = repo_root / record["source"]
            staged = temporary / relative
            staged.parent.mkdir(parents=True, exist_ok=True)
            staged.symlink_to(source.resolve(strict=True))

        manifest = {
            "schema": "owned-game-data-stage-v1",
            "staging_kind": "development symlink overlay; existing owned local inputs; not a fresh disc import",
            "repository_root": str(repo_root),
            "source_roots": source_roots,
            "file_count": len(planned),
            "files": [
                {"path": path, **record}
                for path, record in sorted(planned.items())
            ],
        }
        manifest_path = temporary / MANIFEST_NAME
        with manifest_path.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(manifest, stream, indent=2, sort_keys=True)
            stream.write("\n")
        # Destination was checked absent above; rename gives callers an all-or-none root.
        os.rename(temporary, destination)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return destination

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path, help="new output root; it must not already exist")
    parser.add_argument("--repo-root", type=Path,
                        default=Path(__file__).resolve().parents[1],
                        help="repository containing the ignored owned input trees")
    args = parser.parse_args()
    result = stage_game_data(args.repo_root, args.destination)
    manifest = json.loads((result / MANIFEST_NAME).read_text(encoding="utf-8"))
    print(f"staged {manifest['file_count']} owned files at {result}")
    print(f"manifest: {result / MANIFEST_NAME}")
    print(manifest["staging_kind"])
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
