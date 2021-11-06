import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
spec = importlib.util.spec_from_file_location(
    "verify_dialogue_shadow", Path(__file__).resolve().parents[1] / "tools/verify_dialogue_shadow.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)

class DialogueShadowProvenanceTests(unittest.TestCase):
    def make_fixture(self, root):
        source = root / "source"
        source.mkdir()
        original = source / "original.bin"
        cases = source / "cases.bin"
        elf = root / "SCUS_971.29"
        ram = root / "jam-talk.ram"
        inputs = root / "new-cases.bin"
        producer = root / "okage-oracle"
        for path, data in ((original, b"oracle-output"), (cases, b"fixture"),
                           (elf, b"retail-elf"), (ram, b"snapshot"),
                           (inputs, b"fixture"), (producer, b"producer-build")):
            path.write_bytes(data)
        hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in (original, cases, elf, ram, producer)}
        report = {
            "commands": [[str(producer), "dialogue-shadow", str(elf), str(ram),
                          str(cases), str(original)]],
            "sha256": hashes,
        }
        (source / "report.json").write_text(json.dumps(report))
        return original, inputs, elf, ram, producer, source / "report.json"

    def test_retained_report_proves_source_inputs_and_producer(self):
        with tempfile.TemporaryDirectory() as directory:
            original, inputs, elf, ram, producer, report = self.make_fixture(Path(directory))
            result = verifier.validate_original_provenance(original, inputs, elf, ram)
            self.assertEqual(result["report"], str(report))
            self.assertEqual(result["report_sha256"], hashlib.sha256(report.read_bytes()).hexdigest())
            self.assertEqual(result["producer"]["executable"], str(producer))
            self.assertEqual(result["producer"]["sha256"], hashlib.sha256(producer.read_bytes()).hexdigest())

    def test_rejects_output_hash_from_a_different_original(self):
        with tempfile.TemporaryDirectory() as directory:
            original, inputs, elf, ram, _, report = self.make_fixture(Path(directory))
            original.write_bytes(b"swapped-output")
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                verifier.validate_original_provenance(original, inputs, elf, ram)

    def test_rejects_report_without_matching_oracle_producer(self):
        with tempfile.TemporaryDirectory() as directory:
            original, inputs, elf, ram, _, report = self.make_fixture(Path(directory))
            doc = json.loads(report.read_text())
            doc["commands"][0][-1] = str(original.parent / "other-output.bin")
            report.write_text(json.dumps(doc))
            with self.assertRaisesRegex(ValueError, "unique matching oracle producer"):
                verifier.validate_original_provenance(original, inputs, elf, ram)

if __name__ == "__main__":
    unittest.main()
