import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
import sys
sys.path.insert(0, str(ROOT / 'tools'))
SPEC = importlib.util.spec_from_file_location('import_results_text', ROOT / 'tools/import_results_text.py')
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)

STRINGS = (b'000,000,00', b'0', b'%03d,%03d,%03d',
           b'\\f[1]/\\c[%d]%s\\c[0]%s', b'\\f[1]\\c[%d]%s\\c[0]%s')

def source_fixture():
    segment = bytearray(0x80)
    for address, value in zip(m.STRING_ADDRESSES, STRINGS):
        offset = address - m.STRING_ADDRESSES[0]
        segment[offset:offset + len(value) + 1] = value + b'\0'
    return bytes(segment)

def synthetic_elf(segment=None):
    segment = source_fixture() if segment is None else segment
    phoff, data_offset = 52, 0x80
    image = bytearray(data_offset + len(segment))
    image[:16] = b'\x7fELF\x01\x01\x01' + bytes(9)
    struct.pack_into('<HHIIIIIHHHHHH', image, 16, 2, 8, 1, 0, phoff,
                     0, 0, 52, 32, 1, 40, 0, 0)
    struct.pack_into('<8I', image, phoff, 1, data_offset, m.STRING_ADDRESSES[0],
                     m.STRING_ADDRESSES[0], len(segment), len(segment), 4, 0x100)
    image[data_offset:] = segment
    return bytes(image)

class ResultsTextImporterTests(unittest.TestCase):
    def setUp(self):
        m.ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'

    def import_fixture(self):
        elf = synthetic_elf()
        m.ELF_SHA256 = hashlib.sha256(elf).hexdigest()
        return elf, *m.import_elf(elf)

    def test_extracts_exact_strings_into_pointer_free_slots(self):
        elf, artifact, metadata = self.import_fixture()
        self.assertEqual(len(artifact), 80 + 5 * 64)
        fields = m.HEADER.unpack_from(artifact)
        self.assertEqual(fields[:3], (b'OKRTXT1\0', 1, 320))
        self.assertEqual(fields[3], hashlib.sha256(elf).digest())
        self.assertEqual(fields[4], hashlib.sha256(artifact[80:]).digest())
        for i, value in enumerate(STRINGS):
            slot = artifact[80 + i*64:80 + (i+1)*64]
            self.assertEqual(slot, value + bytes(64-len(value)))
            self.assertEqual(metadata['source_format_strings'][i]['vaddr'],
                             f'0x{m.STRING_ADDRESSES[i]:08x}')

    def test_rejects_unmapped_unterminated_and_non_ascii_sources(self):
        truncated_elf = synthetic_elf(source_fixture()[:20])
        m.ELF_SHA256 = hashlib.sha256(truncated_elf).hexdigest()
        with self.assertRaisesRegex(ValueError, 'Unmapped or ambiguous'):
            m.import_elf(truncated_elf)
        damaged = bytearray(source_fixture())
        start = m.STRING_ADDRESSES[2] - m.STRING_ADDRESSES[0]
        damaged[start:start + 64] = b'X' * 64
        with self.assertRaisesRegex(ValueError, 'Unterminated'):
            m.extract_payload(m.ElfImage(synthetic_elf(bytes(damaged))), bytes(32))
        damaged = bytearray(source_fixture()); damaged[0x40] = 0xff
        with self.assertRaisesRegex(ValueError, 'Non-ASCII'):
            m.extract_payload(m.ElfImage(synthetic_elf(bytes(damaged))), bytes(32))

    def test_elf_hash_pin_and_fresh_output_refusal(self):
        elf, artifact, metadata = self.import_fixture()
        m.ELF_SHA256 = '0' * 64
        with self.assertRaisesRegex(ValueError, 'ELF SHA256'):
            m.import_elf(elf)
        m.ELF_SHA256 = hashlib.sha256(elf).hexdigest()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); target = root / 'results-text.bin'; manifest = root / 'manifest.json'
            m.write_outputs(target, manifest, artifact, metadata)
            self.assertEqual(target.read_bytes(), artifact)
            self.assertEqual(json.loads(manifest.read_text())['payload_sha256'],
                             hashlib.sha256(artifact[80:]).hexdigest())
            with self.assertRaisesRegex(ValueError, 'existing output'):
                m.write_outputs(target, manifest, artifact, metadata)
            alias = root / 'alias'; alias.symlink_to(target)
            with self.assertRaisesRegex(ValueError, 'symlink'):
                m.write_outputs(alias, root / 'other.json', artifact, metadata)

if __name__ == '__main__':
    unittest.main()
