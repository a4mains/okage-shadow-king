"""Pinned retail music-table and class-2 resource mapping checks."""
import importlib.util
from pathlib import Path
import struct
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("import_music_resource", TOOLS / "import_music_resource.py")
resources = importlib.util.module_from_spec(spec)
spec.loader.exec_module(resources)

class MusicResourceImportTests(unittest.TestCase):
    def test_music24_table_decodes_class2_entry(self):
        elf = bytearray(80)
        struct.pack_into("<2I", elf, 0, 0x2000, 0x2010)
        elf[8:16] = b"snt_fnm\0"
        elf[24:26] = b"2\0"
        segments = [
            {"type": 1, "vaddr": "0x001fb5f0", "offset": 0, "filesz": 8},
            {"type": 1, "vaddr": "0x00002000", "offset": 8, "filesz": 64},
        ]
        entry = resources.music_table_entry(bytes(elf), segments, 24)
        self.assertEqual(entry, (0x1fb5f0, 0x2000, 0x2010, "snt_fnm", "2"))

    def test_music24_binds_exact_primary_and_class2_companion_files(self):
        self.assertEqual(resources.RESOURCES[24][0], "SNT_FNM")
        self.assertEqual(set(resources.RESOURCES[24][1]), {
            "SNT_FNM.XSQ", "SNT_FNM0.XV", "SNT_FNM1.XV", "SNT_FPC.XSQ"})
        self.assertEqual(resources.RESOURCE_CLASSES[24], "2")
        self.assertEqual(resources.ALTERNATE_SEQUENCES[24], "SNT_FPC")
        self.assertEqual(resources.RESOURCES[26][0], "SNT_DNM")
        self.assertEqual(set(resources.RESOURCES[26][1]), {
            "SNT_DNM.XSQ", "SNT_DNM0.XV", "SNT_DNM1.XV", "SNT_DPC.XSQ"})
        self.assertEqual(resources.RESOURCE_CLASSES[26], "2")
        self.assertEqual(resources.ALTERNATE_SEQUENCES[26], "SNT_DPC")

    def test_table_strings_must_be_file_backed_and_terminated(self):
        elf = bytearray(16)
        struct.pack_into("<2I", elf, 0, 0x2000, 0x2010)
        segments = [{"type": 1, "vaddr": "0x001fb5f0", "offset": 0, "filesz": 8}]
        with self.assertRaisesRegex(ValueError, "outside file-backed"):
            resources.music_table_entry(bytes(elf), segments, 24)

if __name__ == "__main__":
    unittest.main()
