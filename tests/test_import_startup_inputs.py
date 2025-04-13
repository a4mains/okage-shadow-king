import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import import_startup_inputs as imp  # noqa: E402

# Bytes whose SHA256 is the pinned payload hash (16 little-endian words).
PALETTE = bytes(4) + b"".join(bytes([0xFF, 0xFF, 0xFF, v]) for v in range(0x80, 0x0F, -8))
VA = imp.PALETTE_VA

def build_elf(*, cls=1, data=1, machine=8, phoff=52, phentsize=32, phnum=1, p_offset=0x100,
              p_vaddr=0x200000, p_filesz=0x2000, p_memsz=0x3000, file_size=0x2200,
              payload=PALETTE, truncate_to=None, p_type=1):
    elf = bytearray(file_size)
    elf[0:4] = b"\x7fELF"
    elf[4], elf[5], elf[6] = cls, data, 1
    struct.pack_into("<HHIIIIIHHH", elf, 16, 2, machine, 1, 0x100000, phoff, 0, 0, 52, phentsize, phnum)
    if phoff + phentsize * phnum <= len(elf):
        struct.pack_into("<IIIIIIII", elf, phoff, p_type, p_offset, p_vaddr, p_vaddr, p_filesz, p_memsz, 5, 16)
    at = p_offset + (VA - p_vaddr)
    if 0 <= at and at + 64 <= len(elf):
        elf[at:at + 64] = payload
    return bytes(elf if truncate_to is None else elf[:truncate_to])

class ExtractTests(unittest.TestCase):
    def test_good(self):
        data, record = imp.extract_va(build_elf(), VA, 64)
        self.assertEqual(data, PALETTE)
        self.assertEqual(record["file_offset"], 0x100 + (VA - 0x200000))

    def reject(self, elf, text):
        with self.assertRaises(imp.ImportError_) as caught:
            imp.extract_va(elf, VA, 64)
        self.assertIn(text, str(caught.exception))

    def test_truncated_header(self):
        self.reject(build_elf()[:40], "truncated")

    def test_bad_magic(self):
        self.reject(b"\x00" + build_elf()[1:], "bad magic")

    def test_class64(self):
        self.reject(build_elf(cls=2), "ELFCLASS32")

    def test_big_endian(self):
        self.reject(build_elf(data=2), "little-endian")

    def test_wrong_machine(self):
        self.reject(build_elf(machine=3), "EM_MIPS")

    def test_small_phentsize(self):
        self.reject(build_elf(phentsize=20), "e_phentsize")

    def test_zero_phnum(self):
        self.reject(build_elf(phnum=0), "no program headers")

    def test_phdr_out_of_range(self):
        self.reject(build_elf(phoff=0x2000, phnum=40), "program header table")

    def test_phdr_overflow(self):
        self.reject(build_elf(phoff=0xFFFFFFF0, phnum=1), "program header table")

    def test_phoff_inside_header(self):
        self.reject(build_elf(phoff=48), "e_phoff")

    def test_va_not_covered(self):
        self.reject(build_elf(p_vaddr=0x400000, p_offset=0x100), "no PT_LOAD")

    def test_va_beyond_filesz(self):
        # VA lies in p_memsz (bss) but not in file-backed bytes.
        self.reject(build_elf(p_filesz=0x300, p_memsz=0x4000), "no PT_LOAD")

    def test_va_straddles_end_of_filesz(self):
        self.reject(build_elf(p_filesz=(VA - 0x200000) + 32), "no PT_LOAD")

    def test_non_load_segment_ignored(self):
        self.reject(build_elf(p_type=4), "no PT_LOAD")

    def test_offset_plus_filesz_outside_file(self):
        self.reject(build_elf(p_filesz=0x10000, p_memsz=0x10000), "outside")

    def test_offset_plus_filesz_overflow(self):
        self.reject(build_elf(p_offset=0xFFFFF000, p_filesz=0x2000), "outside")

    def test_vaddr_overflow(self):
        self.reject(build_elf(p_vaddr=0xFFFFF000, p_filesz=0x2000, p_memsz=0x2000), "overflows")

    def test_filesz_gt_memsz(self):
        self.reject(build_elf(p_filesz=0x2000, p_memsz=0x1000), "p_filesz > p_memsz")

    def test_truncated_file_body(self):
        self.reject(build_elf(truncate_to=0x1000), "outside")

class ImportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def write_elf(self, **kwargs):
        path = self.root / "elf"
        path.write_bytes(build_elf(**kwargs))
        return path

    def inputs(self):
        # Real pins cannot be satisfied by synthetic files; patch them for the test.
        fonts = self.root / "fonts"
        effects = self.root / "effects"
        fonts.mkdir(exist_ok=True)
        effects.mkdir(exist_ok=True)
        font_pins, xim_pins = [], []
        for name, _ in imp.FONT_PINS:
            data = name.encode()
            (fonts / name).write_bytes(data)
            font_pins.append((name, hashlib.sha256(data).hexdigest()))
        for name, _ in imp.XIM_PINS:
            data = name.encode()
            (effects / name).write_bytes(data)
            xim_pins.append((name, hashlib.sha256(data).hexdigest()))
        efd = self.root / "TES.EFD"
        efd.write_bytes(b"efd")
        self.saved = (imp.FONT_PINS, imp.XIM_PINS, imp.EFD_SHA256)
        imp.FONT_PINS, imp.XIM_PINS = tuple(font_pins), tuple(xim_pins)
        imp.EFD_SHA256 = hashlib.sha256(b"efd").hexdigest()
        self.addCleanup(self.restore)
        return fonts, effects, efd

    def restore(self):
        imp.FONT_PINS, imp.XIM_PINS, imp.EFD_SHA256 = self.saved

    def test_wrong_elf_hash(self):
        elf = self.write_elf()
        fonts, effects, efd = self.inputs()
        with self.assertRaises(imp.ImportError_) as caught:
            imp.import_startup_inputs(elf, fonts, effects, efd, self.root / "out")
        self.assertIn("does not match the pinned", str(caught.exception))
        self.assertFalse((self.root / "out").exists())

    def test_wrong_payload_hash(self):
        elf = self.write_elf(payload=bytes(64))
        fonts, effects, efd = self.inputs()
        sha = hashlib.sha256(elf.read_bytes()).hexdigest()
        with self.assertRaises(imp.ImportError_) as caught:
            imp.import_startup_inputs(elf, fonts, effects, efd, self.root / "out", expected_elf_sha=sha)
        self.assertIn("payload SHA256", str(caught.exception))
        self.assertFalse((self.root / "out").exists())

    def test_success_and_artifact_shape(self):
        elf = self.write_elf()
        fonts, effects, efd = self.inputs()
        sha = hashlib.sha256(elf.read_bytes()).hexdigest()
        out = self.root / "out"
        manifest = imp.import_startup_inputs(elf, fonts, effects, efd, out, expected_elf_sha=sha)
        self.assertEqual(sorted(p.name for p in (out / "NATIVE_STARTUP").iterdir()),
                         ["FONTCLUT.BIN", "startup-inputs.json"])
        self.assertEqual((out / "NATIVE_STARTUP/FONTCLUT.BIN").read_bytes(), PALETTE)
        self.assertEqual([p.name for p in out.iterdir()], ["NATIVE_STARTUP"])
        on_disk = json.loads((out / "NATIVE_STARTUP/startup-inputs.json").read_text())
        self.assertEqual(on_disk, manifest)
        self.assertEqual(on_disk["schema"], "okage.startup-inputs")
        self.assertEqual(on_disk["version"], 1)
        self.assertEqual(on_disk["source"]["virtual_address"], "0x2003f8")
        self.assertEqual(len(on_disk["inputs"]), 35)
        keys = list(on_disk["inputs"])
        self.assertEqual(keys[:3], ["CMNDATA/FONT/IQ24.FNT", "CMNDATA/FONT/BMUI.FNT", "CMNDATA/FONT/BM.FNT"])
        self.assertEqual(keys[3], "CMNDATA/2D/ENV_TEX.XPF/niji.xim")
        self.assertEqual(keys[-1], "CMNDATA/2D/TES.EFD")
        # The ELF is never copied.
        self.assertNotIn(b"\x7fELF", (out / "NATIVE_STARTUP/FONTCLUT.BIN").read_bytes())

    def test_refuses_existing_out(self):
        elf = self.write_elf()
        fonts, effects, efd = self.inputs()
        sha = hashlib.sha256(elf.read_bytes()).hexdigest()
        out = self.root / "out"
        out.mkdir()
        with self.assertRaises(imp.ImportError_):
            imp.import_startup_inputs(elf, fonts, effects, efd, out, expected_elf_sha=sha)

    def test_font_hash_mismatch_and_missing(self):
        elf = self.write_elf()
        fonts, effects, efd = self.inputs()
        sha = hashlib.sha256(elf.read_bytes()).hexdigest()
        (fonts / "BM.FNT").write_bytes(b"changed")
        with self.assertRaises(imp.ImportError_) as caught:
            imp.import_startup_inputs(elf, fonts, effects, efd, self.root / "o1", expected_elf_sha=sha)
        self.assertIn("expected pinned", str(caught.exception))
        (fonts / "BM.FNT").unlink()
        with self.assertRaises(imp.ImportError_):
            imp.import_startup_inputs(elf, fonts, effects, efd, self.root / "o2", expected_elf_sha=sha)

    def test_xim_and_efd_mismatch(self):
        elf = self.write_elf()
        fonts, effects, efd = self.inputs()
        sha = hashlib.sha256(elf.read_bytes()).hexdigest()
        (effects / "aura.xim").write_bytes(b"x")
        with self.assertRaises(imp.ImportError_):
            imp.import_startup_inputs(elf, fonts, effects, efd, self.root / "o1", expected_elf_sha=sha)
        (effects / "aura.xim").write_bytes(b"aura.xim")
        efd.write_bytes(b"other")
        with self.assertRaises(imp.ImportError_):
            imp.import_startup_inputs(elf, fonts, effects, efd, self.root / "o2", expected_elf_sha=sha)

    def test_pins_shape(self):
        self.assertEqual(len(imp.XIM_PINS), 31)
        self.assertEqual([n for n, _ in imp.FONT_PINS], ["IQ24.FNT", "BMUI.FNT", "BM.FNT"])
        self.assertEqual(PALETTE_HASH(), imp.PALETTE_SHA256)

    def test_cli_has_no_hash_override(self):
        with self.assertRaises(SystemExit):
            imp.main(["--elf", "a", "--fonts", "b", "--effects", "c", "--efd", "d", "--out", "e",
                      "--expected-elf-sha", "00"])

def PALETTE_HASH():
    return hashlib.sha256(PALETTE).hexdigest()

if __name__ == "__main__":
    unittest.main()
