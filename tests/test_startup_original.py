import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('startup_original_report', ROOT / 'tools' / 'startup_original_report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)

class NibbleMaskTests(unittest.TestCase):
    def test_untouched_byte_is_unwritten(self):
        self.assertEqual(report.nibble_mask(0x00, 0xff), (0, 0, 0))

    def test_fully_written_byte_any_value(self):
        # Written values equal to either fill are still detected because the runs have complementary fills.
        for value in (0x00, 0xff, 0xa5, 0x0f, 0xf0):
            self.assertEqual(report.nibble_mask(value, value), (3, value, 0))

    def test_single_lane_writes(self):
        self.assertEqual(report.nibble_mask(0x07, 0xf7), (1, 0x07, 0))   # low nibble written (value 7)
        self.assertEqual(report.nibble_mask(0x90, 0x9f), (2, 0x90, 0))   # high nibble written (value 9)
        self.assertEqual(report.nibble_mask(0x00, 0x0f), (2, 0x00, 0))   # high written as 0, low untouched
        self.assertEqual(report.nibble_mask(0x0f, 0x0f), (3, 0x0f, 0))   # both agree

    def test_partially_written_nibble_is_an_anomaly_and_unwritten(self):
        m, v, anomalies = report.nibble_mask(0x03, 0xf3 ^ 0x1)
        self.assertEqual((m & 1, anomalies), (0, 1))

    def test_lcg_steps(self):
        s = 1
        for _ in range(5):
            s = (s * 0x41c64e6d + 0x3039) & 0xffffffff
        self.assertEqual(report.lcg_steps(1, s), 5)
        self.assertEqual(report.lcg_steps(1, 1), 0)

if __name__ == '__main__':
    unittest.main()
