#!/usr/bin/env python3
"""Differential validation tests comparing Okage native GS texture memory against PCSX2 GSLocalMemory reference.

Pinned PCSX2 revision: 9fc90907c29294ac9db9aadd2d6c6bdf33403571
Reference file: /Users/user/src/games/pcsx2/pcsx2/GS/GSLocalMemory.h
"""
import json
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN_CANDS = [
    Path(os.environ.get("OKAGE_GS_DIFF_COMPARE", "")),
    ROOT / "build/c2p-release/okage-gs-differential-compare",
    ROOT / "build/field-release/okage-gs-differential-compare",
    ROOT / "build/native-release/okage-gs-differential-compare",
]

def find_binary():
    return next((c for c in BIN_CANDS if c.is_file()), None)

class GsLocalMemoryDifferentialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bin = find_binary()
        if cls.bin is None:
            raise unittest.SkipTest("okage-gs-differential-compare binary not found")

    def run_comparator(self, *args):
        cmd = [str(self.bin), *args]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=30.0)
        try:
            doc = json.loads(p.stdout)
        except json.JSONDecodeError as exc:
            self.fail(f"Comparator output is not valid JSON: {exc}\nStdout: {p.stdout}\nStderr: {p.stderr}")
        return p.returncode, doc, p.stderr

    def test_baseline_differential_passes(self):
        """Standard differential execution: all cases must agree exactly with PCSX2 reference."""
        ret, doc, stderr = self.run_comparator("--json")
        self.assertEqual(ret, 0, f"Expected clean pass, got exit {ret}: {stderr}")
        self.assertEqual(doc["status"], "PASS")
        self.assertEqual(doc["pcsx2_revision"], "9fc90907c29294ac9db9aadd2d6c6bdf33403571")
        self.assertGreaterEqual(doc["cases_run"], 7)
        self.assertFalse(doc["negative_control_active"])
        self.assertIsNone(doc["first_failure"])
        self.assertIn("PSMT8H (0x1b) high-byte lane 3 writes with preservation of low 24 bits (lanes 0..2)", doc["coverage"])

    def test_negative_control_clobber_low24(self):
        """Negative control: simulate low-24 bits clobber during high-byte write."""
        ret, doc, stderr = self.run_comparator("--defect-clobber-low24")
        self.assertEqual(ret, 1)
        self.assertEqual(doc["status"], "FAIL")
        self.assertTrue(doc["negative_control_active"])
        ff = doc["first_failure"]
        self.assertIsNotNone(ff)
        self.assertEqual(ff["defect_type"], "low24_lane_clobbered")
        self.assertEqual(ff["address"], "0x0")

    def test_negative_control_wrong_lane(self):
        """Negative control: simulate write to wrong byte lane."""
        ret, doc, stderr = self.run_comparator("--defect-wrong-lane")
        self.assertEqual(ret, 1)
        self.assertEqual(doc["status"], "FAIL")
        self.assertTrue(doc["negative_control_active"])
        ff = doc["first_failure"]
        self.assertIsNotNone(ff)
        self.assertEqual(ff["defect_type"], "high_byte_mismatch")

    def test_negative_control_linear_addressing(self):
        """Negative control: simulate linear addressing (unswizzled) defect."""
        ret, doc, stderr = self.run_comparator("--defect-linear-addr")
        self.assertEqual(ret, 1)
        self.assertEqual(doc["status"], "FAIL")
        self.assertTrue(doc["negative_control_active"])
        ff = doc["first_failure"]
        self.assertIsNotNone(ff)
        self.assertEqual(ff["defect_type"], "high_byte_mismatch")

    def test_negative_control_no_wrap(self):
        """Negative control: simulate failure to wrap modulo 4 MiB."""
        ret, doc, stderr = self.run_comparator("--defect-no-wrap")
        self.assertEqual(ret, 1)
        self.assertEqual(doc["status"], "FAIL")
        self.assertTrue(doc["negative_control_active"])
        ff = doc["first_failure"]
        self.assertIsNotNone(ff)
        self.assertEqual(ff["defect_type"], "wrapping_mismatch")

    def test_negative_control_state_leak(self):
        """Negative control: simulate state corruption across sequence steps."""
        ret, doc, stderr = self.run_comparator("--defect-state-leak")
        self.assertEqual(ret, 1)
        self.assertEqual(doc["status"], "FAIL")
        self.assertTrue(doc["negative_control_active"])
        ff = doc["first_failure"]
        self.assertIsNotNone(ff)
        self.assertEqual(ff["defect_type"], "persistent_sequence_mismatch")

if __name__ == "__main__":
    unittest.main()
