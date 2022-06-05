import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
SPEC = importlib.util.spec_from_file_location('import_battle_ui', ROOT / 'tools/import_battle_ui.py')
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)

def source_fixture():
    records = []
    for i, spec in enumerate(m.EXPECTED):
        words = [((i + 1) * 0x1020304 + j * 0x11111) & 0xffffffff for j in range(28)]
        words[0] = spec['stream']
        words[1:4] = [40, 6, 2]
        words[21] = spec['atlas']
        words[22] = spec['ctor']
        words[23] = spec['callback']
        words[24] = spec['type']
        words[25] = spec['capacity_a']
        words[26] = spec['capacity_b']
        words[27] = 0
        records.extend(words)
    return struct.pack('<56I', *records) + bytes(4)

def synthetic_elf(table, duplicate=False):
    phoff = 52
    data_offset = 0x100
    segment_size = len(table)
    header = bytearray(data_offset + segment_size)
    header[:16] = b'\x7fELF\x01\x01\x01' + bytes(9)
    struct.pack_into('<HHIIIIIHHHHHH', header, 16,
                     2, 8, 1, 0, phoff, 0, 0, 52, 32, 1, 40, 0, 0)
    struct.pack_into('<8I', header, phoff, 1, data_offset, m.TABLE_ADDRESS,
                     m.TABLE_ADDRESS, segment_size, segment_size, 4, 0x100)
    header[data_offset:data_offset + segment_size] = table
    if duplicate:
        # A second load segment maps the same virtual extent from another file offset.
        second_offset = len(header)
        header.extend(table)
        struct.pack_into('<H', header, 44, 2)
        struct.pack_into('<8I', header, phoff + 32, 1, second_offset,
                         m.TABLE_ADDRESS, m.TABLE_ADDRESS, segment_size,
                         segment_size, 4, 0x100)
    return bytes(header)

class BattleUIImporterTests(unittest.TestCase):
    def setUp(self):
        m.ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'

    def test_elf_mapping_and_pointer_free_import_layout(self):
        table = source_fixture()
        elf = synthetic_elf(table)
        image = m.ElfImage(elf)
        self.assertEqual(image.read(m.TABLE_ADDRESS, len(table)), table)
        digest = hashlib.sha256(elf).hexdigest()
        m.ELF_SHA256 = digest
        output, manifest = m.import_elf(elf)
        fields = m.HEADER.unpack_from(output)
        self.assertEqual(fields[:5], (b'OKBTUI1\0', 1, 2, 28, 112))
        self.assertEqual(fields[5], bytes.fromhex(digest))
        self.assertEqual(fields[6], hashlib.sha256(table).digest())
        self.assertEqual(len(output), 88 + 2 * 112)
        row0 = list(struct.unpack_from('<28I', output, 88))
        row1 = list(struct.unpack_from('<28I', output, 88 + 112))
        source_words = struct.unpack('<56I', table[:224])
        for row, src, stream_id, handler in [(row0, source_words[:28], 1, 1),
                                               (row1, source_words[28:], 2, 2)]:
            self.assertEqual(row[0], stream_id)
            self.assertEqual(row[1:21], list(src[1:21]))
            self.assertEqual(row[21:24], [1, handler, handler])
            self.assertEqual(row[24:], list(src[24:]))
        self.assertEqual(manifest['table_sha256'], hashlib.sha256(table).hexdigest())
        self.assertEqual(manifest['table_bytes_including_terminator'], 228)

    def test_rejects_bad_terminator_pointers_and_unsupported_layout(self):
        table = bytearray(source_fixture())
        table[-1] = 1
        with self.assertRaisesRegex(ValueError, 'terminator'):
            m.extract_table(m.ElfImage(synthetic_elf(bytes(table))))
        table = bytearray(source_fixture())
        struct.pack_into('<I', table, 0, 0x1fd824)
        with self.assertRaisesRegex(ValueError, 'stream'):
            m.extract_table(m.ElfImage(synthetic_elf(bytes(table))))
        table = bytearray(source_fixture())
        struct.pack_into('<I', table, 112 + 92, 0x1684d4)
        with self.assertRaisesRegex(ValueError, 'callback'):
            m.extract_table(m.ElfImage(synthetic_elf(bytes(table))))

    def test_elf_reader_refuses_unmapped_ambiguous_and_truncated_ranges(self):
        image = m.ElfImage(synthetic_elf(source_fixture()))
        with self.assertRaisesRegex(ValueError, 'Unmapped or ambiguous'):
            image.read(m.TABLE_ADDRESS - 1, 4)
        with self.assertRaisesRegex(ValueError, 'Unmapped or ambiguous'):
            image.read(m.TABLE_ADDRESS, m.TERMINATED_TABLE_SIZE + 1)
        duplicate = m.ElfImage(synthetic_elf(source_fixture(), duplicate=True))
        with self.assertRaisesRegex(ValueError, 'Unmapped or ambiguous'):
            duplicate.read(m.TABLE_ADDRESS, m.TERMINATED_TABLE_SIZE)

    def test_hash_pin_and_writer_freshness(self):
        elf = synthetic_elf(source_fixture())
        m.ELF_SHA256 = '0' * 64
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            m.import_elf(elf)
        m.ELF_SHA256 = hashlib.sha256(elf).hexdigest()
        output, manifest = m.import_elf(elf)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / 'battle-ui.bin'
            meta = root / 'manifest.json'
            m.write_outputs(target, meta, output, manifest)
            self.assertEqual(target.read_bytes(), output)
            self.assertEqual(json.loads(meta.read_text())['table_sha256'], manifest['table_sha256'])
            with self.assertRaisesRegex(ValueError, 'existing output'):
                m.write_outputs(target, meta, output, manifest)
            alias = root / 'alias'
            alias.symlink_to(target)
            with self.assertRaisesRegex(ValueError, 'symlink'):
                m.write_outputs(alias, root / 'other.json', output, manifest)

    def test_cli_rejects_nonowned_elf_without_outputs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'unowned.elf'
            output = root / 'ui.bin'
            source.write_bytes(b'not an owned ELF')
            result = subprocess.run([sys.executable, str(ROOT / 'tools/import_battle_ui.py'),
                                     '--elf', str(source), '--output', str(output)],
                                    capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertFalse(output.exists())
            self.assertFalse(output.with_name(output.name + '.manifest.json').exists())

if __name__ == '__main__':
    unittest.main()
