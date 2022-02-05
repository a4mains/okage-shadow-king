import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'verify_game_variables_reset',
    Path(__file__).resolve().parents[1] / 'tools/verify_game_variables_reset.py')
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)

def hand_authored_output(inputs):
    """Literal byte layout, independent of the verifier's record packing loop."""
    initialized = bytearray(0xd78)
    fields = {
        0x00e: '01', 0x51c: 'e8030000', 0x570: '03000000', 0x588: '03000000',
        0x5c4: '0100aa00010000000000000000000500',
        0x614: '0100ab00050000000000000000000500',
        0x664: '0100ac000a0000000000000000000500',
        0x6b4: '0100ad00190000000000000000000500',
        0x704: '0100ae001e0000000000000000000500',
        0x754: '0100af00280000000000000000000500',
        0x7a4: '0100b000010000000000000000000500',
        0x844: '41726900', 0x864: '3f3f3f00',
    }
    for offset, hex_value in fields.items():
        value = bytes.fromhex(hex_value)
        initialized[offset:offset + len(value)] = value
    return b''.join(inputs[at:at + 16] + initialized + inputs[at + 16 + 0xd78:at + 0x1020]
                    for at in range(0, len(inputs), 0x1020))

class GameVariablesResetVerifierTests(unittest.TestCase):
    def test_deterministic_fixture_fills_and_canaries(self):
        data = verifier.fixtures()
        self.assertEqual(len(data), 4 * 0x1020)
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         'cf6f6de6a4bf3da5111927f3bbbe678ae2aa27314e03967a9de03759ac1a91f0')
        rows = verifier.rows(data)
        for row, fill in zip(rows[:3], (0, 255, 165)):
            self.assertEqual(row[16:4112], bytes([fill]) * 4096)
        self.assertEqual(set(rows[3][16:4112]), set(range(256)))
        for row in rows:
            self.assertNotIn(0, row[:16] + row[-16:])
        self.assertEqual(len({row[:16] for row in rows}), 4)

    def test_literal_layout_and_preserved_bytes(self):
        inputs = verifier.fixtures()
        output = hand_authored_output(inputs)
        result = verifier.compare(inputs, output, output)
        self.assertTrue(result['exact'])
        self.assertTrue(result['independent_expected_layout'])
        self.assertEqual(result['compared_bytes_per_output'], 16512)
        self.assertEqual(result['preserved_bytes_per_row'], 680)
        self.assertEqual(result['sha256']['input'], hashlib.sha256(inputs).hexdigest())
        self.assertEqual(result['sha256']['native'], hashlib.sha256(output).hexdigest())

    def test_accepts_one_and_sixteen_rows_without_requiring_generator_identity(self):
        for count in (1, 16):
            inputs = bytes((i * 11) % 256 for i in range(count * 0x1020))
            output = hand_authored_output(inputs)
            self.assertEqual(verifier.compare(inputs, output, output)['rows'], count)

    def test_rejects_empty_partial_and_over_limit_input(self):
        for data in (b'', b'\0', b'\0' * 0x101f, b'\0' * 0x1021,
                     b'\0' * (17 * 0x1020)):
            with self.subTest(size=len(data)), self.assertRaisesRegex(ValueError, 'input extent'):
                verifier.compare(data, data, data)
        with self.assertRaisesRegex(ValueError, 'row extent'):
            verifier.expected_row(b'\0' * 0x1000)

    def test_rejects_output_extent_on_either_side(self):
        inputs = verifier.fixtures()
        output = hand_authored_output(inputs)
        for bad in (b'', output[:-1], output[:-0x1020], output + b'x', output + output[:0x1020]):
            for original, native in ((bad, output), (output, bad)):
                with self.subTest(size=len(bad)), self.assertRaisesRegex(ValueError, 'output extent'):
                    verifier.compare(inputs, original, native)

    def test_rejects_identical_corruption_in_defaults_padding_and_reset_edges(self):
        inputs = verifier.fixtures()
        output = hand_authored_output(inputs)
        offsets = (0, 0xe, 0xf, 0x51c, 0x51f, 0x570, 0x574, 0x588,
                   0x5c4, 0x5c5, 0x5c7, 0x5c9, 0x5d2, 0x5d3,
                   0x7a8, 0x7f3, 0x844, 0x847, 0x848, 0x864, 0x867,
                   0x877, 0x878, 0xd77)
        for root_offset in offsets:
            bad = bytearray(output)
            bad[16 + root_offset] ^= 1
            for original, native in ((bad, output), (output, bad), (bad, bad)):
                with self.subTest(root_offset=root_offset), self.assertRaisesRegex(ValueError, 'row 0 root'):
                    verifier.compare(inputs, original, native)

    def test_rejects_canary_and_tail_corruption_with_useful_location(self):
        inputs = verifier.fixtures()
        output = hand_authored_output(inputs)
        for offset, label in ((0, 'prefix canary'), (15, 'prefix canary'),
                              (16 + 0xd78, 'preserved root tail'),
                              (16 + 0xfff, 'preserved root tail'),
                              (0x1010, 'suffix canary'), (0x101f, 'suffix canary')):
            bad = bytearray(output)
            bad[3 * 0x1020 + offset] ^= 1
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, 'row 3 ' + label):
                verifier.compare(inputs, bad, bad)

    def test_rejects_reordered_or_duplicate_output_rows(self):
        inputs = verifier.fixtures()
        output = hand_authored_output(inputs)
        for bad in (output[0x1020:0x2040] + output[:0x1020] + output[0x2040:],
                    output[:0x1020] * 4):
            with self.assertRaisesRegex(ValueError, 'row [01] prefix canary'):
                verifier.compare(inputs, bad, bad)

    def test_fresh_outputs_refuse_existing_files_and_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'nested/input.bin'
            verifier.write_fresh(target, b'first')
            for value in (b'first', b'second'):
                with self.assertRaises(FileExistsError):
                    verifier.write_fresh(target, value)
            self.assertEqual(target.read_bytes(), b'first')
            for name, destination in (('link', target), ('dangling', target.parent / 'missing')):
                link = target.parent / name
                link.symlink_to(destination)
                with self.assertRaises(FileExistsError):
                    verifier.write_fresh(link, b'new')
            self.assertFalse((target.parent / 'missing').exists())

if __name__ == '__main__':
    unittest.main()
