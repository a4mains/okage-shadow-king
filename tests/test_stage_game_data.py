import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from tools.stage_game_data import (
    MANIFEST_NAME,
    SOURCE_LAYOUT,
    stage_game_data,
)

class StageGameDataTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name) / "repo"
        self.repo.mkdir()
        for source, _, _ in SOURCE_LAYOUT:
            (self.repo / source).mkdir(parents=True)

    def put(self, source_root, relative, content):
        path = self.repo / source_root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def test_merges_runtime_layout_with_hashes_and_reversible_links(self):
        decoded = self.put("local/xpf-decoded", "FLDDATA/FLDSET.XDB", b"field")
        font = self.put("local/font-input/CMNDATA/FONT", "IQ24.FNT", b"font")
        table = self.put("local/text-input/CMNDATA/XDB", "BKDATA.XDB", b"table")
        message = self.put("local/text-input/FLDDATA/MES", "FIELD.PME", b"messages")
        effect = self.put("local/effect-input/CMNDATA/2D", "TES.EFD", b"effects")
        audio = self.put("local/audio-input/SNDDATA", "BGM/JGL_BSC0.XV", b"audio")
        native_ui = self.put("local/battle-input-v6", "NAME_ENTRY.bin", b"ui")
        destination = Path(self.temp.name) / "run" / "game-data"

        result = stage_game_data(self.repo, destination)

        expected = {
            "FLDDATA/FLDSET.XDB": decoded,
            "CMNDATA/FONT/IQ24.FNT": font,
            "CMNDATA/XDB/BKDATA.XDB": table,
            "FLDDATA/MES/FIELD.PME": message,
            "CMNDATA/2D/TES.EFD": effect,
            "SNDDATA/BGM/JGL_BSC0.XV": audio,
            "NATIVE_UI/NAME_ENTRY.bin": native_ui,
        }
        for relative, source in expected.items():
            staged = result / relative
            self.assertTrue(staged.is_symlink(), relative)
            self.assertEqual(staged.resolve(), source.resolve())
            self.assertEqual(staged.read_bytes(), source.read_bytes())

        manifest = json.loads((result / MANIFEST_NAME).read_text())
        self.assertEqual(manifest["schema"], "owned-game-data-stage-v1")
        self.assertIn("not a fresh disc import", manifest["staging_kind"])
        self.assertEqual(manifest["file_count"], len(expected))
        records = {item["path"]: item for item in manifest["files"]}
        self.assertEqual(set(records), set(expected))
        for relative, source in expected.items():
            self.assertEqual(records[relative]["sha256"], hashlib.sha256(source.read_bytes()).hexdigest())
            self.assertEqual(records[relative]["source"], source.relative_to(self.repo).as_posix())

    def test_identical_overlay_is_deduplicated_with_both_sources_recorded(self):
        first = self.put("local/xpf-decoded", "CMNDATA/XDB/BKDATA.XDB", b"same")
        second = self.put("local/text-input/CMNDATA/XDB", "BKDATA.XDB", b"same")
        destination = Path(self.temp.name) / "game-data"

        stage_game_data(self.repo, destination)

        manifest = json.loads((destination / MANIFEST_NAME).read_text())
        record = next(item for item in manifest["files"] if item["path"] == "CMNDATA/XDB/BKDATA.XDB")
        self.assertEqual(record["source"], first.relative_to(self.repo).as_posix())
        self.assertEqual(record["identical_sources"], [{
            "path": second.relative_to(self.repo).as_posix(),
            "label": "owned_tables",
        }])

    def test_conflicting_overlay_fails_before_creating_destination(self):
        self.put("local/xpf-decoded", "CMNDATA/XDB/BKDATA.XDB", b"decoded")
        self.put("local/text-input/CMNDATA/XDB", "BKDATA.XDB", b"different")
        destination = Path(self.temp.name) / "game-data"

        with self.assertRaisesRegex(ValueError, "conflicting assets target CMNDATA/XDB/BKDATA.XDB"):
            stage_game_data(self.repo, destination)
        self.assertFalse(destination.exists())

    def test_existing_destination_is_never_overwritten(self):
        destination = Path(self.temp.name) / "game-data"
        destination.mkdir()
        marker = destination / "keep.txt"
        marker.write_text("keep")

        with self.assertRaises(FileExistsError):
            stage_game_data(self.repo, destination)
        self.assertEqual(marker.read_text(), "keep")

    def test_destination_inside_input_tree_is_rejected(self):
        destination = self.repo / "local/xpf-decoded/staged"
        with self.assertRaisesRegex(ValueError, "overlaps a source tree"):
            stage_game_data(self.repo, destination)
        self.assertFalse(destination.exists())

if __name__ == "__main__":
    unittest.main()
