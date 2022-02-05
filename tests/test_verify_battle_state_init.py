import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'verify_battle_state_init', Path(__file__).resolve().parents[1] / 'tools/verify_battle_state_init.py')
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)

class BattleStateInitVerifierTests(unittest.TestCase):
    def fixture(self):
        defaults = bytes(range(1, 17))
        prefix, suffix = bytes([0x31]) * 32, bytes([0x92]) * 32
        initial = bytes([0xa5]) * 0x81d0
        record = (bytes(0x40) + defaults + bytes(0x10) + b'\xff\xff\xff\xff\0\0\xff\xff' +
                  bytes(0x106) + b'\xa5\xa5' + bytes(4) +
                  (b'\xff\xff' + bytes(6)) * 16 + bytes(11) + b'\x80')
        self.assertEqual(len(record), 512)
        root = initial[:0x50] + bytes(4) + initial[0x54:0x60] + record * 64 + initial[0x8060:]
        first = prefix + root + suffix
        second_root = root[:0x80a4] + bytes([255]) * 256 + bytes(4) + b'\x01\0\0\0' + root[0x81ac:]
        return defaults + prefix + initial + suffix, first + prefix + second_root + suffix

    def test_generator_reproduces_retained_fixture(self):
        data = verifier.fixtures()
        self.assertEqual(len(verifier.rows(data)), 4)
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         '8a549cb73e7d8e07b72134ffd07f7d59ebd5170ef20c6b497ae74982cad3a15a')

    def test_two_phases_and_nonzero_explicit_defaults_pass(self):
        inputs, output = self.fixture()
        result = verifier.compare(inputs, output, output)
        self.assertEqual(result['compared_bytes'], 2 * verifier.SNAPSHOT_SIZE)
        self.assertTrue(result['full_backing_and_canaries'])

    def test_rejects_malformed_or_excessive_input(self):
        inputs, output = self.fixture()
        for data in (b'', inputs[:-1], inputs + b'x', inputs * 17):
            with self.subTest(length=len(data)), self.assertRaisesRegex(ValueError, 'input extent'):
                verifier.compare(data, output, output)

    def test_rejects_output_extents_on_either_side(self):
        inputs, output = self.fixture()
        for bad in (b'', output[:-1], output[:-verifier.SNAPSHOT_SIZE], output + b'x', output + output[:verifier.SNAPSHOT_SIZE]):
            for original, native in ((bad, output), (output, bad)):
                with self.subTest(length=len(bad)), self.assertRaisesRegex(ValueError, 'output extent'):
                    verifier.compare(inputs, original, native)

    def test_rejects_defaults_unknown_headers_gaps_and_canary_corruption(self):
        inputs, output = self.fixture()
        for phase in (0, 1):
            for offset in (0, 31, 32, 32 + 0x54, 32 + 0x5f, 32 + 0xa0,
                           32 + 0x1ce, 32 + 0x1cf, 32 + 0x805f, 32 + 0x8060,
                           32 + 0x81ac, verifier.SNAPSHOT_SIZE - 1):
                bad = bytearray(output)
                bad[phase * verifier.SNAPSHOT_SIZE + offset] ^= 1
                for original, native in ((bad, output), (output, bad), (bad, bad)):
                    with self.subTest(phase=phase, offset=offset), self.assertRaisesRegex(ValueError, 'phase'):
                        verifier.compare(inputs, original, native)

    def test_rejects_swapped_or_duplicated_phases(self):
        inputs, output = self.fixture()
        first, second = output[:verifier.SNAPSHOT_SIZE], output[verifier.SNAPSHOT_SIZE:]
        for bad in (second + first, first + first, second + second):
            with self.assertRaisesRegex(ValueError, 'phase'):
                verifier.compare(inputs, bad, bad)

    def test_rejects_default_input_mismatch_even_if_outputs_agree(self):
        inputs, output = self.fixture()
        changed = bytes([inputs[0] ^ 1]) + inputs[1:]
        with self.assertRaisesRegex(ValueError, 'root\\+0xa0'):
            verifier.compare(changed, output, output)

    def test_writer_preserves_existing_results(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'fixture.bin'
            verifier.write_exact(target, b'first')
            verifier.write_exact(target, b'first')
            with self.assertRaisesRegex(ValueError, 'different existing'):
                verifier.write_exact(target, b'second')
            self.assertEqual(target.read_bytes(), b'first')
            link = Path(directory) / 'link'
            link.symlink_to(target)
            with self.assertRaisesRegex(ValueError, 'symlink'):
                verifier.write_exact(link, b'first')

if __name__ == '__main__':
    unittest.main()
