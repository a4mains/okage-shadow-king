#!/usr/bin/env python3
"""Import the seven direct chapter-card XIM files from the owned USA disc.

The importer copies CMNDATA/2D/CHP_01.XIM through CHP_07.XIM byte-for-byte.
It verifies the retail disc hash, the retained ISO inventory's path/LBA/size,
each source file hash, and the basic XIM payload geometry before writing a new
output directory containing the images and a provenance manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile

from inspect_disc import Disc, sha256 as sha256_file

DISC_SHA256 = "08623a1400e2610aecd0247ae7440c97501495c6f819f675494e44de20b62d2a"
ELF_SHA256 = "2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5"
SOURCE_DIRECTORY = "CMNDATA/2D"
SOURCE_ROWS = (
    ("CHP_01.XIM", 81497, "94ef3841f0b5f1c619b9cb92bc332bf443cdd0b776537f307f320a3d26b16b97"),
    ("CHP_02.XIM", 81546, "a1fe9e2b346fb47c1346e955d609f09c33824e2cd4dbfc71b67e3fff1b51c3a3"),
    ("CHP_03.XIM", 81595, "c6688818294ca4a4eefef0ce69b24c21a181ddf8567255ba5c557bb5e12325f2"),
    ("CHP_04.XIM", 81644, "3dfb30c2ba4443fdb9a25a61871f3791d9cb4e6471f55b0055b20bfd88ea7877"),
    ("CHP_05.XIM", 81693, "4908a7fe391f1616a921628a6ae9159f9df921792c2e4e3939b83ba559d2ec2f"),
    ("CHP_06.XIM", 81742, "87cb672ea4d78631405cbe261f0c8c2cc4802cf53b0dee3c11c9e551368d9deb"),
    ("CHP_07.XIM", 81791, "5e452ff322ff6c1c35e57083269faa83f794dd3b53868a018c616ded24a0d5a0"),
)
FILE_SIZE = 99376
EXPECTED_XIM = {
    "psm": 19,
    "cpsm": 0,
    "palette_count": 1,
    "colors": 256,
    "palette_block_bytes": 1040,
    "width": 512,
    "height": 192,
    "pixel_bytes": 98304,
}

def parse_xim(data: bytes) -> dict[str, int]:
    """Read and validate chapter-image format and exact image extent."""
    if len(data) < 32:
        raise ValueError("Truncated XIM header")
    psm = (struct.unpack_from("<I", data, 0)[0] >> 20) & 63
    cpsm = (struct.unpack_from("<I", data, 4)[0] >> 19) & 15
    if psm < 16:
        raise ValueError("Chapter XIM must contain an indexed image")
    palette_block_bytes = struct.unpack_from("<I", data, 16)[0]
    palette_count, colors = struct.unpack_from("<II", data, 24)
    if palette_count != 1 or colors not in (16, 256):
        raise ValueError("Unsupported XIM palette count or size")
    if cpsm not in (0, 2):
        raise ValueError("Unsupported XIM palette format")
    expected_palette_bytes = 16 + colors * (2 if cpsm == 2 else 4)
    if palette_block_bytes != expected_palette_bytes:
        raise ValueError("Invalid XIM palette block extent")
    block = 16 + palette_block_bytes
    if block + 16 > len(data):
        raise ValueError("Missing XIM pixel block")
    height, width = struct.unpack_from("<II", data, block + 8)
    if not width or not height or width > 4096 or height > 4096:
        raise ValueError("Invalid XIM dimensions")
    bits = {19: 8, 20: 4, 27: 8}.get(psm)
    if bits is None:
        raise ValueError(f"Unsupported chapter XIM PSM: {psm}")
    pixel_bytes = (width * height * bits + 7) // 8
    if len(data) - (block + 16) != pixel_bytes:
        raise ValueError("XIM dimensions do not match pixel payload")
    return {
        "psm": psm,
        "cpsm": cpsm,
        "palette_count": palette_count,
        "colors": colors,
        "palette_block_bytes": palette_block_bytes,
        "width": width,
        "height": height,
        "pixel_bytes": pixel_bytes,
    }

def read_chapter_images(disc_path: Path, index_path: Path) -> tuple[list[tuple[dict, bytes, dict]], dict]:
    """Read exact pinned chapter files and return payloads plus provenance."""
    disc_path = Path(disc_path)
    if disc_path.is_symlink() or not disc_path.is_file():
        raise ValueError("Disc input must be an ordinary file, not a symlink")
    actual_disc_hash = sha256_file(disc_path)
    if actual_disc_hash != DISC_SHA256:
        raise ValueError("Disc SHA256 does not match supported owned USA image")
    index = json.loads(Path(index_path).read_text(encoding="utf-8"))
    if index.get("image_sha256") != DISC_SHA256 or index.get("elf", {}).get("sha256") != ELF_SHA256:
        raise ValueError("Disc inventory provenance differs from supported owned USA image")
    indexed = {entry.get("path"): entry for entry in index.get("files", [])}
    source_records = []
    for number, (name, lba, expected_hash) in enumerate(SOURCE_ROWS, start=1):
        path = f"{SOURCE_DIRECTORY}/{name}"
        entry = indexed.get(path)
        expected_entry = {"path": path, "lba": lba, "size": FILE_SIZE}
        if entry is None or {key: entry.get(key) for key in expected_entry} != expected_entry:
            raise ValueError(f"Owned ISO index extent differs for {path}")
        source_records.append((number, entry, expected_hash))

    disc = Disc(disc_path)
    try:
        inventory = {entry["path"]: entry for entry in disc.inventory()}
        found_chapters = sorted(
            path for path in inventory
            if path.startswith(f"{SOURCE_DIRECTORY}/CHP_") and path.upper().endswith(".XIM")
        )
        expected_chapters = [f"{SOURCE_DIRECTORY}/{name}" for name, _, _ in SOURCE_ROWS]
        if found_chapters != expected_chapters:
            raise ValueError("Owned ISO chapter-image inventory differs from reviewed CHP_01..CHP_07 set")
        imported = []
        for number, indexed_entry, expected_hash in source_records:
            path = indexed_entry["path"]
            entry = inventory.get(path)
            if entry != indexed_entry:
                raise ValueError(f"ISO directory extent differs from retained index for {path}")
            payload = disc.read(entry["lba"], entry["size"])
            actual_hash = hashlib.sha256(payload).hexdigest()
            if actual_hash != expected_hash:
                raise ValueError(f"{path} SHA256 differs from reviewed owned source")
            xim = parse_xim(payload)
            if xim != EXPECTED_XIM:
                raise ValueError(f"{path} XIM metadata differs from reviewed chapter image format")
            imported.append((entry, payload, xim))
    finally:
        disc.close()
    return imported, {"path": str(disc_path), "sha256": actual_disc_hash}

def write_bundle(output_dir: Path, imported: list[tuple[dict, bytes, dict]], disc_source: dict,
                 index_path: Path) -> Path:
    """Create a fresh output directory atomically; never replace existing files."""
    output_dir = Path(os.path.abspath(output_dir))
    if output_dir.is_symlink() or output_dir.exists():
        raise FileExistsError(f"Output directory already exists or is a symlink: {output_dir}")
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    parent = output_dir.parent.resolve(strict=True)
    if not parent.is_dir():
        raise ValueError("Output parent directory must be a directory")
    target = parent / output_dir.name
    temp = Path(tempfile.mkdtemp(prefix=f".{target.name}.", dir=parent))
    created_target = False
    manifest = {
        "schema": "okage-owned-chapter-images-v1",
        "scope": "Direct ISO files copied byte-for-byte from the owned USA disc; no XPF extraction or conversion.",
        "disc": {**disc_source, "elf_sha256": ELF_SHA256},
        "iso_index": {"path": str(index_path), "disc_sha256": DISC_SHA256},
        "images": [],
    }
    try:
        for entry, payload, xim in imported:
            name = Path(entry["path"]).name
            with (temp / name).open("xb") as stream:
                stream.write(payload)
            manifest["images"].append({
                "source_path": entry["path"],
                "lba": entry["lba"],
                "source_bytes": entry["size"],
                "source_sha256": hashlib.sha256(payload).hexdigest(),
                "output_path": name,
                "output_sha256": hashlib.sha256(payload).hexdigest(),
                "xim": xim,
            })
        with (temp / "manifest.json").open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(manifest, stream, indent=2, sort_keys=True)
            stream.write("\n")
        os.mkdir(target)
        created_target = True
        for child in temp.iterdir():
            os.rename(child, target / child.name)
        temp.rmdir()
    except BaseException:
        shutil.rmtree(temp, ignore_errors=True)
        if created_target:
            shutil.rmtree(target, ignore_errors=True)
        raise
    return target

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disc", type=Path, required=True, help="Hash-pinned owned ISO/PS2 disc image")
    parser.add_argument("--index", type=Path, default=Path("docs/disc-report.json"),
                        help="Retained hash-pinned ISO inventory")
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="Fresh output directory; existing paths are never replaced")
    args = parser.parse_args(argv)
    imported, disc_source = read_chapter_images(args.disc, args.index)
    result = write_bundle(args.output_dir, imported, disc_source, args.index)
    print(f"Imported {len(imported)} chapter images to {result}")
    for entry, payload, xim in imported:
        print(f"{entry['path']}: {xim['width']}x{xim['height']} PSM {xim['psm']} CPSM {xim['cpsm']} "
              f"{len(payload)} bytes SHA256 {hashlib.sha256(payload).hexdigest()}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
