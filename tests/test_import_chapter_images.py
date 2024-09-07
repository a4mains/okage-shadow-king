import struct
import tempfile
import unittest
from pathlib import Path

from import_chapter_images import EXPECTED_XIM, parse_xim, write_bundle

def make_xim(psm=19, cpsm=0, width=2, height=1, colors=256):
    palette_bytes = 16 + colors * (2 if cpsm == 2 else 4)
    data = bytearray(16 + palette_bytes + 16)
    struct.pack_into("<I", data, 0, psm << 20)
    struct.pack_into("<I", data, 4, cpsm << 19)
    struct.pack_into("<IIII", data, 16, palette_bytes, 0, 1, colors)
    block = 16 + palette_bytes
    struct.pack_into("<II", data, block + 8, height, width)
    bits = {19: 8, 20: 4, 27: 8}[psm]
    data.extend(bytes((width * height * bits + 7) // 8))
    return bytes(data)

class ChapterImageImportTests(unittest.TestCase):
    def test_parses_retail_chapter_image_metadata(self):
        data = make_xim(width=512, height=192)
        self.assertEqual(parse_xim(data), EXPECTED_XIM)

    def test_rejects_truncated_header(self):
        with self.assertRaisesRegex(ValueError, "Truncated XIM header"):
            parse_xim(bytes(31))

    def test_rejects_invalid_palette_extent(self):
        data = bytearray(make_xim())
        struct.pack_into("<I", data, 16, 17)
        with self.assertRaisesRegex(ValueError, "palette block extent"):
            parse_xim(bytes(data))

    def test_rejects_payload_length_mismatch(self):
        with self.assertRaisesRegex(ValueError, "pixel payload"):
            parse_xim(make_xim()[:-1])

    def test_bundle_refuses_existing_output(self):
        entry = {"path": "CMNDATA/2D/CHP_01.XIM", "lba": 81497, "size": 1}
        imported = [(entry, b"x", {"psm": 19})]
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "chapter-assets"
            output.mkdir()
            with self.assertRaises(FileExistsError):
                write_bundle(output, imported, {"path": "disc", "sha256": "a"}, Path("index.json"))

if __name__ == "__main__":
    unittest.main()
