#!/usr/bin/env python3
"""Import the RNG startup inputs: the 64-byte font CLUT from the retail ELF.

Reads the owned retail executable only to extract 64 bytes at virtual address
0x2003f8 (the 16-entry glyph palette, the startup file manifest),
verifies the ELF, the payload, and the hash-bound startup font/XIM/EFD inputs
against pins, and writes NATIVE_STARTUP/FONTCLUT.BIN plus
NATIVE_STARTUP/startup-inputs.json. The ELF itself is never copied.
See the startup input RNG contract.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
from typing import Optional

ELF_SHA256 = "2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5"
PALETTE_VA = 0x2003F8
PALETTE_SIZE = 64
PALETTE_SHA256 = "2ea415986ee6ffc70ebf023d6324670a18967408615dd04455a23cf49cd9a31c"
EFD_SHA256 = "2c99993d5ac51760c426b4b157fb65c1348c85d09ef2552fc47e5241b3e43c6c"

PAYLOAD_PATH = "NATIVE_STARTUP/FONTCLUT.BIN"
MANIFEST_PATH = "NATIVE_STARTUP/startup-inputs.json"

# the startup file manifest (flags order IQ24, BMUI, BM) file sha256.
FONT_PINS = (
    ("IQ24.FNT", "f38eb6266c02315dda5b6a4f97d1dab347ef67c7b262f23deb4a3575d4cadc7a"),
    ("BMUI.FNT", "ebf4eb1f157638baf3ad9002e9990acb24b3807f54653a8c34a8d4e2ef5df356"),
    ("BM.FNT", "7bc478129d2e46991cab24b563ff02af0e0c5b32294f70bb475cf5a31e5241fb"),
)
# the authored startup file order: authored order (0x221bf8) file sha256.
XIM_PINS = (
    ("niji.xim", "b86509274c38f73e3c811402fd43ce1a434a5a9687ff8bc0b00f5a8511eb6b03"),
    ("splash_a.xim", "b133efc1b4a928478d573ea79b3f78db6c0c9298e0d323337aaa01846997d6af"),
    ("wat_4.xim", "90acd2d41837a60b50773a694fd60c3dfc828b4241dc636d3f8f348d52ad1ae6"),
    ("exp9.xim", "0f75f7b7bd0b3c218cdaaeb482bdaafe6ff7e999c6b7d986e1fbdbbc2da3df46"),
    ("f2.xim", "bea1730e29afffe4f97f9f229fc679f85148e57b91e74a96870c404f93aa7a3e"),
    ("h29_4.xim", "a683d6bfbe5fef31a398cb762443520271367ef14509dcae0dbdf2d706ae9717"),
    ("hit!.xim", "4ca368c7882ba6c67fb31b77fd2abc8ef48c6ca991c93f658a618b9dadabcb26"),
    ("spark.xim", "b8d181a1157a6aaa1ddf7653a959eb82ebfab3dad1fbb6c905ba1b3d22240e24"),
    ("smoke_4.xim", "52f59a2114feab46c535f56d729e33194b748fbaeb08ce3b8a3c1c29d3629a63"),
    ("thunder.xim", "82283a13f1cfaf107d740540f84d8c48b7e3bfb9de7c4a182ae51b8f67d39e80"),
    ("oldskilt4.xim", "473ab019281a39cba4b43e1043010d6cf6c02f21af658974902943da392fd6ad"),
    ("water.xim", "a550cb4932f2bc8a5406361c42ec90467b2689d143126e927479338785d0bd3b"),
    ("suuji.xim", "83e0896ecca63c95ea69858d7e93ce9ada5a5198880ae8e0c29ae2148e9d4fef"),
    ("lgtballm4.xim", "81d1ee7bddd13c218cf3a85672e76b5e673c895acf44b1251a52ba64accc7b5b"),
    ("fuki32b.xim", "f4362dc85edeb174ba4d012d2cad6b666bfef3f420e05366352a8b08306195a4"),
    ("all.xim", "e35e6d02436cde092dc26cffd3340cdfa5119173ffe02f080797b288d9fd1947"),
    ("all2.xim", "af4d452b82ceec1e21a01aa2885aedab6453b281e3005fd44b92c299e5ae0e6a"),
    ("kemuri.xim", "c04b3c2219b95ce79b524afa5c2d7cf9f363b4d51d6459bfadb00a118c4ccb4a"),
    ("thunderball.xim", "ec934adf9c3e0fcfa3365e1e1af1f404471d2a6266df582f1d9352ad41b39086"),
    ("6eff01.xim", "57fc8e2e47a985d35ba5531058bd5b490a5d268498122393525f56caa94ac61d"),
    ("6eff02.xim", "f67a1bdd275a54284539246e89cfb2f138bcd9f59c12fb3343867aec5c648d33"),
    ("5sen.xim", "a6f4aabe8909a195ff37735efaf72001287c37958f5c68fd4d6cd498b42a775b"),
    ("exp_c.xim", "bf48faf54ce36ea1af13844e715c4b0123e713675e2cb5e56610924a3c3cd724"),
    ("heart.xim", "821a7e98c4774c2bc00f8a0786072032bab3967100c3136279797d3506de15a0"),
    ("onnpu.xim", "b9294c102f58b85d552145aedada0ffffdad90abffdfb0a14d344ccbb2e58a1c"),
    ("wind.xim", "16354ff48e492e46dec87ba99d85295c210efe7ea7daac63ec6a0f7423d0eab5"),
    ("treasure.xim", "e9ccaa79db2df0a8c6b0858ee41415c8b0e81be45c3d034622f5947d51710af1"),
    ("page.xim", "869aae415f2ec8ed6a18b7b874dbf3e5054b57cb1fba7630403da35da5bc641b"),
    ("odoro.xim", "1b19fb7af3dab17670a9873ea0d70902a40e9e2bb374e0d5ebc53f02667b5503"),
    ("aura.xim", "9b789518dc328b3462e3bc406e66ce61c18d639ae6c4464450e0d79d2daece4a"),
    ("kyusyu16.xim", "fc34044f98269d70c193a38bdbaad4ceef3e221ce3f351b69d6ff78699cc883a"),
)

PT_LOAD = 1
ET_EXEC = 2
EM_MIPS = 8

class ImportError_(Exception):
    pass

def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

def extract_va(elf: bytes, va: int, size: int) -> tuple[bytes, dict]:
    """Return (bytes, source record) for [va, va+size) from one PT_LOAD file image."""
    if len(elf) < 52:
        raise ImportError_("ELF truncated: shorter than the 52-byte ELF32 header")
    if elf[:4] != b"\x7fELF":
        raise ImportError_("not an ELF image (bad magic)")
    if elf[4] != 1:
        raise ImportError_(f"not ELFCLASS32 (EI_CLASS={elf[4]})")
    if elf[5] != 1:
        raise ImportError_(f"not little-endian (EI_DATA={elf[5]})")
    if elf[6] != 1:
        raise ImportError_(f"unexpected EI_VERSION {elf[6]}")
    e_type, e_machine, _e_version, _e_entry, e_phoff, _e_shoff, _e_flags, e_ehsize, e_phentsize, e_phnum = \
        struct.unpack_from("<HHIIIIIHHH", elf, 16)
    if e_machine != EM_MIPS:
        raise ImportError_(f"not EM_MIPS (e_machine={e_machine})")
    if e_phentsize < 32:
        raise ImportError_(f"implausible e_phentsize {e_phentsize}")
    if e_phnum == 0:
        raise ImportError_("ELF has no program headers")
    if e_phoff < 52:
        raise ImportError_(f"e_phoff {e_phoff} overlaps the ELF header")
    table_end = e_phoff + e_phentsize * e_phnum
    if table_end > len(elf):
        raise ImportError_(
            f"program header table [{e_phoff}, {table_end}) lies outside the {len(elf)}-byte file")
    end_va = va + size
    for index in range(e_phnum):
        base = e_phoff + index * e_phentsize
        p_type, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, _p_flags, _p_align = \
            struct.unpack_from("<IIIIIIII", elf, base)
        if p_type != PT_LOAD:
            continue
        if p_offset + p_filesz > len(elf) or p_offset + p_filesz > 0xFFFFFFFF:
            raise ImportError_(
                f"PT_LOAD segment {index} file range [{p_offset}, {p_offset + p_filesz}) is "
                f"outside the {len(elf)}-byte file or overflows 32 bits")
        if p_vaddr + p_filesz > 0xFFFFFFFF:
            raise ImportError_(f"PT_LOAD segment {index} virtual range overflows 32 bits")
        if p_filesz > p_memsz:
            raise ImportError_(f"PT_LOAD segment {index} has p_filesz > p_memsz")
        if p_vaddr <= va and end_va <= p_vaddr + p_filesz:
            offset = p_offset + (va - p_vaddr)
            if offset + size > len(elf):
                raise ImportError_("computed payload range is outside the file")
            record = {
                "index": index,
                "p_offset": p_offset,
                "p_vaddr": p_vaddr,
                "p_filesz": p_filesz,
                "p_memsz": p_memsz,
                "file_offset": offset,
            }
            return elf[offset:offset + size], record
    raise ImportError_(
        f"no PT_LOAD segment covers VA 0x{va:x}..0x{end_va:x} entirely within p_filesz")

def read_pinned(path: Path, expected: str, what: str) -> None:
    if not path.is_file():
        raise ImportError_(f"{what}: missing input file {path}")
    actual = sha256_file(path)
    if actual != expected:
        raise ImportError_(f"{what}: {path} has SHA256 {actual}, expected pinned {expected}")

def import_startup_inputs(elf_path: Path, fonts: Path, effects: Path, efd: Path, out: Path,
                          *, expected_elf_sha: str = ELF_SHA256) -> dict:
    """Build the artifact. expected_elf_sha is overridable for tests only; the CLI never passes it."""
    out = out.expanduser().absolute()
    if out.exists() or out.is_symlink():
        raise ImportError_(f"--out already exists (refusing to overwrite): {out}")
    if not elf_path.is_file():
        raise ImportError_(f"ELF not found: {elf_path}")
    elf = elf_path.read_bytes()
    elf_hash = sha256_bytes(elf)
    if elf_hash != expected_elf_sha:
        raise ImportError_(f"ELF SHA256 {elf_hash} does not match the pinned {expected_elf_sha}")
    payload, segment = extract_va(elf, PALETTE_VA, PALETTE_SIZE)
    if len(payload) != PALETTE_SIZE:
        raise ImportError_("extracted payload has the wrong size")
    payload_hash = sha256_bytes(payload)
    if payload_hash != PALETTE_SHA256:
        raise ImportError_(f"payload SHA256 {payload_hash} does not match the pinned {PALETTE_SHA256}")

    inputs: dict[str, str] = {}
    for name, expected in FONT_PINS:
        read_pinned(fonts / name, expected, "font")
        inputs[f"CMNDATA/FONT/{name}"] = expected
    for name, expected in XIM_PINS:
        read_pinned(effects / name, expected, "effect texture")
        inputs[f"CMNDATA/2D/ENV_TEX.XPF/{name}"] = expected
    read_pinned(efd, EFD_SHA256, "effect script")
    inputs["CMNDATA/2D/TES.EFD"] = EFD_SHA256

    manifest = {
        "schema": "okage.startup-inputs",
        "version": 1,
        "source": {
            "elf_sha256": elf_hash,
            "virtual_address": f"0x{PALETTE_VA:x}",
            "file_offset": segment["file_offset"],
            "size": PALETTE_SIZE,
            "segment": {k: segment[k] for k in ("index", "p_offset", "p_vaddr", "p_filesz", "p_memsz")},
        },
        "payload": {PAYLOAD_PATH: payload_hash},
        "inputs": inputs,
        "importer": {
            "tool": "tools/import_startup_inputs.py",
            "tool_sha256": sha256_file(Path(__file__).resolve()),
        },
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{out.name}.import-", dir=out.parent))
    try:
        directory = temporary / "NATIVE_STARTUP"
        directory.mkdir()
        with (directory / "FONTCLUT.BIN").open("xb") as stream:
            stream.write(payload)
        with (directory / "startup-inputs.json").open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(manifest, stream, indent=2)
            stream.write("\n")
        os.chmod(temporary, 0o755)
        os.rename(temporary, out)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return manifest

def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--elf", type=Path, required=True, help="owned SCUS_971.29 (read only)")
    parser.add_argument("--fonts", type=Path, required=True, help="directory with IQ24.FNT BMUI.FNT BM.FNT")
    parser.add_argument("--effects", type=Path, required=True,
                        help="directory with the 31 ENV_TEX.XPF member .xim files")
    parser.add_argument("--efd", type=Path, required=True, help="owned TES.EFD")
    parser.add_argument("--out", type=Path, required=True, help="fresh output directory (must not exist)")
    args = parser.parse_args(argv)
    try:
        manifest = import_startup_inputs(args.elf, args.fonts, args.effects, args.efd, args.out)
    except ImportError_ as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"wrote {args.out}/{PAYLOAD_PATH} ({PALETTE_SIZE} bytes, sha256 {manifest['payload'][PAYLOAD_PATH]})")
    print(f"wrote {args.out}/{MANIFEST_PATH} ({len(manifest['inputs'])} hash-bound inputs)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
