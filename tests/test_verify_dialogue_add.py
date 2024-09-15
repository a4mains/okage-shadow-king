import struct
import unittest

from tools import verify_dialogue_add as probe

def synthetic_output(inputs):
    records = []
    returns = []
    native_records = []
    for case in probe.parse_cases(inputs):
        variables, handler_return = probe.expected(case)
        original_scalars = [0] * 13
        for index in (0, 1, 5):
            original_scalars[index] = 1
        original_scalars = tuple(original_scalars)
        records.append(struct.pack("<I", case[0]) + variables + struct.pack("<13I", *original_scalars))
        native_scalars = tuple(original_scalars[index] for index in probe.NATIVE_FIELD_PROJECTION)
        native_records.append(struct.pack("<I", case[0]) + variables + struct.pack("<10I", *native_scalars))
        returns.append(struct.pack("<2I", case[0], handler_return))
    return b"".join(records), b"".join(returns), b"".join(native_records)

class DialogueAddProbeTests(unittest.TestCase):
    def test_fixture_covers_wrap_noop_and_handler_order(self):
        cases = probe.parse_cases(probe.fixture())
        self.assertEqual(len(cases), 11)
        self.assertEqual(probe.expected(cases[0])[0][0x410 + 101], 0x80)
        self.assertEqual(probe.expected(cases[1])[0][0x510 + 101 * 4:0x514 + 101 * 4],
                         bytes.fromhex("00000080"))
        self.assertEqual(probe.expected(cases[2])[0][0x10 + 101 // 8], 0xa1)
        self.assertEqual(probe.expected(cases[3])[0][0x110 + 101 // 8], 0xa1)
        self.assertEqual(probe.expected(cases[6])[1], 7)
        self.assertEqual(probe.expected(cases[7])[1], 12)
        self.assertEqual(probe.expected(cases[10])[1], 7)

    def test_verifies_complete_variables_and_output_shape(self):
        inputs = probe.fixture()
        output, returns, _ = synthetic_output(inputs)
        report = probe.verify(inputs, output, returns)
        self.assertEqual(len(report["cases"]), 11)
        self.assertIsNone(report["exact_native_match"])

    def test_rejects_variable_corruption_and_handler_return_mismatch(self):
        inputs = probe.fixture()
        output, returns, _ = synthetic_output(inputs)
        output = bytearray(output)
        output[4 + 0x410 + 101] ^= 1
        with self.assertRaisesRegex(ValueError, "variable bank"):
            probe.verify(inputs, bytes(output), returns)
        output, returns, _ = synthetic_output(inputs)
        returns = bytearray(returns)
        returns[4] ^= 1
        with self.assertRaisesRegex(ValueError, "handler return"):
            probe.verify(inputs, output, bytes(returns))

    def test_native_comparison_includes_controller_scalars(self):
        inputs = probe.fixture()
        original, returns, native = synthetic_output(inputs)
        native = bytearray(native)
        native[4 + probe.VARIABLE_BYTES] ^= 1
        with self.assertRaisesRegex(ValueError, "controller scalar"):
            probe.verify(inputs, original, returns, bytes(native))

    def test_native_projection_matches_shared_fields_only(self):
        inputs = probe.fixture()
        original, returns, native = synthetic_output(inputs)
        report = probe.verify(inputs, original, returns, native)
        self.assertTrue(report["exact_native_match"])
        self.assertEqual(report["native_field_projection"], [2, 3, 4, 6, 7, 8, 9, 10, 11, 12])

    def test_original_only_metadata_must_remain_at_fixture_values(self):
        inputs = probe.fixture()
        original, returns, _ = synthetic_output(inputs)
        original = bytearray(original)
        original[4 + probe.VARIABLE_BYTES + 5 * 4] = 0
        with self.assertRaisesRegex(ValueError, "fields 0/1/5"):
            probe.verify(inputs, bytes(original), returns)

    def test_fixture_rejects_out_of_scope_indices(self):
        inputs = bytearray(probe.fixture())
        struct.pack_into("<I", inputs, 12, 0x200)
        with self.assertRaisesRegex(ValueError, "invalid selectors"):
            probe.parse_cases(bytes(inputs))

if __name__ == "__main__":
    unittest.main()
