import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import verify_battle_capture as capture

def input_case(mode=2, rgba=0x80808080, progress=0, field=0, steps=2, reserved=0):
    return struct.pack('<6I', mode, rgba, progress, field, steps, reserved)

class BattleCaptureVerifierTests(unittest.TestCase):
    def test_generated_cases_cover_requested_branches_and_scale_boundary(self):
        data = capture.fixtures()
        cases = capture.rows(data)
        self.assertEqual(len(cases), len(capture.fixture_cases()))
        self.assertEqual(len(data), len(cases) * capture.INPUT_ROW_SIZE)
        long_cases = [case for case in cases if case['steps'] == 220]
        self.assertEqual({case['branch'] for case in long_cases}, {0, 1, 4})
        self.assertEqual({case['field_nonzero'] for case in long_cases}, {0, 1})
        boundary = {case['progress'] for case in cases if case['steps'] == 1}
        self.assertTrue({0xfffe7fff, 0xfffe8000, 0xfffe8001} <= boundary)
        self.assertIn(0xfffffe02, [case['mode'] for case in cases])
        self.assertTrue({0x7fffffff, 0x80000000, 0xffffffff} <=
                        {case['progress'] for case in cases})
        low_byte_modes = [case for case in cases if case['mode'] & 0xff in (0, 1, 3, 4)]
        self.assertEqual({case['mode'] & 0xff for case in low_byte_modes}, {0, 1, 3, 4})
        for mode in (0, 1, 3, 4):
            with self.subTest(low_byte_mode=mode):
                self.assertEqual({case['field_nonzero'] for case in low_byte_modes
                                  if case['mode'] & 0xff == mode}, {0, 1})
        self.assertEqual(capture.fixtures(), data)
        self.assertEqual(hashlib.sha256(data).hexdigest(), hashlib.sha256(capture.fixtures()).hexdigest())

    def test_input_extent_and_header_bounds(self):
        for data in (b'', input_case()[:-1], input_case() + b'\0', input_case() * 65):
            with self.subTest(length=len(data)), self.assertRaisesRegex(ValueError, 'input extent'):
                capture.rows(data)
        for field, steps, reserved in ((2, 1, 0), (0, 0, 0), (0, 2001, 0), (1, 1, 1)):
            with self.subTest(field=field, steps=steps, reserved=reserved), self.assertRaisesRegex(ValueError, 'header bounds'):
                capture.rows(input_case(field=field, steps=steps, reserved=reserved))
        self.assertEqual(len(capture.rows(input_case(steps=2000) * 64)), 64)

    def test_exact_extent_counts_hashes_and_scope(self):
        inputs = input_case(steps=2) + input_case(mode=0x102, field=1, steps=3)
        output = bytes(range(80)) * 5
        result = capture.compare(inputs, output, output)
        self.assertTrue(result['exact'])
        self.assertEqual((result['case_count'], result['total_steps'], result['compared_bytes']), (2, 5, 400))
        self.assertEqual(result['expected_output_bytes'], 400)
        self.assertIsNone(result['first_difference'])
        self.assertIn('does not launch', result['scope'])
        self.assertEqual(result['sha256']['native'], hashlib.sha256(output).hexdigest())

    def test_rejects_truncated_or_extra_outputs_on_both_sides(self):
        inputs = input_case(steps=2)
        output = bytes(160)
        for name in ('original', 'native'):
            for invalid in (b'', output[:-1], output + bytes(1)):
                pair = {'original': output, 'native': output}
                pair[name] = invalid
                with self.subTest(name=name, length=len(invalid)), self.assertRaisesRegex(ValueError, name + ' output extent'):
                    capture.compare(inputs, **pair)

    def test_first_difference_reports_state_and_vertex_words(self):
        inputs = input_case(steps=1) + input_case(steps=2)
        original = bytes(240)
        for step, word, value, expected in (
                (0, 0, 0x80000002, {'case': 0, 'step': 0, 'kind': 'state', 'field': 'mode'}),
                (1, 4, 0xffffffff, {'case': 1, 'step': 0, 'kind': 'vertex', 'vertex': 0,
                                    'coordinate': 'uv', 'u32_half': 'low'}),
                (2, 19, 0x12345678, {'case': 1, 'step': 1, 'kind': 'vertex', 'vertex': 3,
                                     'coordinate': 'xyz', 'u32_half': 'high'})):
            native = bytearray(original)
            struct.pack_into('<I', native, step * capture.OUTPUT_STEP_SIZE + word * 4, value)
            result = capture.compare(inputs, original, native)
            self.assertFalse(result['exact'])
            diff = result['first_difference']
            for key, expected_value in expected.items():
                self.assertEqual(diff[key], expected_value)
            self.assertEqual(diff['original_word'], '0x00000000')
            self.assertEqual(diff['native_word'], f'0x{value:08x}')

    def test_fresh_writer_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'cases.bin'
            capture.write_fresh(path, b'first')
            with self.assertRaises(FileExistsError):
                capture.write_fresh(path, b'replacement')
            self.assertEqual(path.read_bytes(), b'first')

if __name__ == '__main__':
    unittest.main()
