import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'verify_event_shorts', Path(__file__).resolve().parents[1] / 'tools/verify_event_shorts.py')
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)

class EventShortVerifierTests(unittest.TestCase):
    def fixture(self):
        # Hand-authored outputs: read initial -23131, SET32767, ADD1 -> -32768.
        inputs = b''.join(struct.pack('<3I', *row) for row in
                          ((0, 0xe0007, 0), (1, 0xe0007, 32767), (2, 0xe0007, 1)))
        output = bytearray()
        storage = bytearray([0xa5] * 4096)
        for value, low in ((-23131, b'\xa5\xa5'), (32767, b'\xff\x7f'), (-32768, b'\x00\x80')):
            storage[0x7b2:0x7b4] = low
            output += struct.pack('<i', value) + storage
        return inputs, bytes(output)

    def test_generator_reproduces_executed_corpus(self):
        data = verifier.fixtures()
        self.assertEqual(len(verifier.rows(data)), 676)
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         '57959d872b7d7d0e45448d1dad1c8ccd3337a29d96154c492c15e461ec596d1e')

    def test_signed_chain_and_full_backing_pass(self):
        inputs, output = self.fixture()
        result = verifier.compare(inputs, output, output)
        self.assertEqual(result['rows'], 3)
        self.assertEqual(result['compared_bytes'], 3 * 4100)
        self.assertTrue(result['arithmetic_and_full_backing'])

    def test_rejects_malformed_input_and_unsupported_selectors(self):
        inputs, output = self.fixture()
        for bad in (b'', inputs[:-1], inputs + b'x', b'\0' * (12 * 1025),
                    struct.pack('<3I', 3, 0x80000, 0),
                    *(struct.pack('<3I', 0, selector, 0) for selector in
                      (0x70000, 0x100000, 0x180000, 0x80028, 0xf0028, 0x8ffff))):
            with self.subTest(input=bad[:12]), self.assertRaises(ValueError):
                verifier.compare(bad, output, output)

    def test_rejects_empty_truncated_and_extra_output_on_both_sides(self):
        inputs, output = self.fixture()
        for bad in (b'', output[:-1], output[:-4100], output + b'x', output + output[:4100]):
            for original, native in ((bad, output), (output, bad)):
                with self.subTest(length=len(bad)), self.assertRaisesRegex(ValueError, 'output extent'):
                    verifier.compare(inputs, original, native)

    def test_rejects_getter_sign_and_adjacent_or_distant_storage_corruption(self):
        inputs, output = self.fixture()
        # Sign extension, selected bytes, adjacent bytes, far sentinel and late row.
        for at in (3, 4 + 0x7b2, 4 + 0x7b1, 4 + 0x7b4, 4, 4100 * 2 + 4099):
            bad = bytearray(output)
            bad[at] ^= 1
            for original, native in ((bad, output), (output, bad), (bad, bad)):
                with self.subTest(offset=at), self.assertRaisesRegex(ValueError, 'row'):
                    verifier.compare(inputs, original, native)

    def test_rejects_reordered_rows_despite_equal_original_and_native(self):
        inputs, output = self.fixture()
        bad = output[4100:8200] + output[:4100] + output[8200:]
        with self.assertRaisesRegex(ValueError, 'row 0'):
            verifier.compare(inputs, bad, bad)

    def test_generator_refuses_destructive_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'fixture.bin'
            verifier.write_exact(target, b'first')
            verifier.write_exact(target, b'first')
            with self.assertRaisesRegex(ValueError, 'different existing'):
                verifier.write_exact(target, b'second')
            self.assertEqual(target.read_bytes(), b'first')
            link = Path(directory) / 'link.bin'
            link.symlink_to(target)
            with self.assertRaisesRegex(ValueError, 'symlink'):
                verifier.write_exact(link, b'first')

if __name__ == '__main__':
    unittest.main()
