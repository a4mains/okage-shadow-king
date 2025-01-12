import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("gpu_address_poc", Path(__file__).resolve().parents[1] / "tools/run_gpu_address_poc.py")
poc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(poc)

class VerdictTests(unittest.TestCase):
    def fixture(self, directory, ratios):
        binary = directory / "fake-binary"
        binary.write_text("test fixture, never executed")
        metadata = {"binary": str(binary), "binary_sha256": poc.sha(binary),
                    "protocol_sha256": poc.sha(poc.PROTOCOL), "input_hashes": {}, "source_hashes": {},
                    "retained_corpus_hashes": poc.PINNED}
        (directory / "build-metadata.json").write_text(json.dumps(metadata))
        identity = poc.prepared_identity(directory)
        workloads = {f"mixed-{n}": n for n in (0, 1, 31, 33)}
        workloads.update({f"synthetic-psm{p}-{n}": n for n in (4096, 16384, 143360, 1048576) for p in (0, 19)})
        workloads["retained-corpus-356160"] = 356160
        for trial, ratio in enumerate(ratios):
            rows = [{"type": "correctness", "rows": 356160, "different_results": 0}]
            for name, count in workloads.items():
                for pair in range(31):
                    rows.append({"type": "sample", "trial": trial, "workload": name, "count": count,
                                 "input_bytes": count * 20, "output_bytes": count * 8, "pair": pair,
                                 "cpu_first": (pair + trial) % 2 == 0,
                                 "cpu_s": (ratio if count == 4096 else .5) * (100 if trial == 4 else 1),
                                 "gpu_e2e_s": float(100 if trial == 4 else 1), "gpu_command_s": None})
            (directory / f"trial-{trial}.jsonl").write_text("\n".join(map(json.dumps, rows)))
            (directory / f"trial-{trial}-eligibility.json").write_text(json.dumps({"eligible": True, "reasons": [],
                "identity": identity, "raw_sha256": poc.sha(directory / f"trial-{trial}.jsonl")}))

    def test_four_of_five_and_median_trial_ratio(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture(directory, [1.2, 1.21, 1.3, 1.4, .01])
            result = poc.summarize(directory)
            self.assertTrue(result["speedup_demonstrated"])
            row = next(r for r in result["workloads"] if r["workload"] == "synthetic-psm0-4096")
            self.assertEqual(row["aggregate_ratio"], 1.21)
            self.assertEqual(row["passing_trials"], 4)
            self.assertIsNone(row["gpu_command_median_s"])

    def test_three_good_trials_cannot_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture(directory, [4, 4, 4, 1.19, 1.19])
            self.assertFalse(poc.summarize(directory)["speedup_demonstrated"])

    def test_contamination_and_missing_coverage_cannot_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture(directory, [2] * 5)
            eligibility_path = directory / "trial-2-eligibility.json"
            eligibility = json.loads(eligibility_path.read_text())
            eligibility.update(eligible=False, reasons=["user input"])
            eligibility_path.write_text(json.dumps(eligibility))
            with self.assertRaisesRegex(RuntimeError, "contaminated"):
                poc.summarize(directory)
            eligibility.update(eligible=True, reasons=[])
            eligibility_path.write_text(json.dumps(eligibility))
            path = directory / "trial-4.jsonl"
            lines = path.read_text().splitlines()
            path.write_text("\n".join(lines[:-1]))
            with self.assertRaisesRegex(RuntimeError, "raw trial changed"):
                poc.summarize(directory)

    def test_rebuilt_binary_cannot_be_mixed_with_old_trials(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture(directory, [2] * 5)
            (directory / "fake-binary").write_text("different executable")
            with self.assertRaisesRegex(RuntimeError, "executable changed"):
                poc.summarize(directory)

    def test_missing_trial_cannot_be_synthesized(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture(directory, [2] * 5)
            (directory / "trial-4.jsonl").unlink()
            with self.assertRaises(FileNotFoundError):
                poc.summarize(directory)

    def test_invalid_gpu_timestamp_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture(directory, [2] * 5)
            path = directory / "trial-0.jsonl"
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            rows[1]["gpu_command_s"] = 0.
            path.write_text("\n".join(map(json.dumps, rows)))
            eligibility_path = directory / "trial-0-eligibility.json"
            eligibility = json.loads(eligibility_path.read_text())
            eligibility["raw_sha256"] = poc.sha(path)
            eligibility_path.write_text(json.dumps(eligibility))
            with self.assertRaisesRegex(RuntimeError, "invalid GPU command"):
                poc.summarize(directory)
