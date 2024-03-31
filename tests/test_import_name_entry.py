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
SPEC = importlib.util.spec_from_file_location('import_name_entry', ROOT / 'tools/import_name_entry.py')
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)

SOURCE_WORDS = (
    0x1f8978, 25, 40, 1, 0xc39f8000, 0xc29e0000, 0x43828cd9, 0x3f800000,
    0, 0xbfc00000, 0, 0, 0, 0x43100000, 0, 0, 0x53f0167d, 0x53f0167d,
    0x53780348, 0x53780348, 1, 0x1f88c8, 0x1196a0, 0x119700,
    9, 32, 16, 0,
)
ROW_SOURCE = struct.pack('<28I', *SOURCE_WORDS) + bytes(4)
ATLAS_SOURCE = bytes(16)
DOUBLE_SOURCE = bytes.fromhex(
    'd7a3703d0ad7d33f' * 4)
DEFAULT_NAME_SOURCE = b'Ari\0'
STRING_SOURCES = (b' \0', b'\x81\x40\0')

def static_source():
    raw = bytearray(0x33)  # 0x222a80 through 0x222ab2, inclusive.
    for address, value in zip(m.STRING_ADDRESSES, STRING_SOURCES):
        raw[address - 0x222a80:address - 0x222a80 + len(value)] = value
    for address, value in zip(m.DOUBLE_ADDRESSES,
                              (DOUBLE_SOURCE[i:i+8] for i in range(0, 32, 8))):
        raw[address - 0x222a80:address - 0x222a80 + 8] = value
    raw[m.DEFAULT_NAME_ADDRESS - 0x222a80:
        m.DEFAULT_NAME_ADDRESS - 0x222a80 + 4] = DEFAULT_NAME_SOURCE
    return bytes(raw)

def source_segments():
    segments = [
        (m.ROW_ADDRESS, ROW_SOURCE),
        (m.ATLAS_ADDRESS, ATLAS_SOURCE),
        (0x222a80, static_source()),
    ]
    return tuple(segments)

def synthetic_elf(segments=None, duplicate=False):
    segments = list(source_segments() if segments is None else segments)
    phoff = 52
    phentsize = 32
    data_offset = (phoff + len(segments) * phentsize + 15) & ~15
    offsets = []
    total = data_offset
    for _, content in segments:
        offsets.append(total)
        total += len(content)
    image = bytearray(total)
    image[:16] = b'\x7fELF\x01\x01\x01' + bytes(9)
    struct.pack_into('<HHIIIIIHHHHHH', image, 16, 2, 8, 1, 0, phoff,
                     0, 0, 52, phentsize, len(segments), 40, 0, 0)
    for i, ((address, content), offset) in enumerate(zip(segments, offsets)):
        struct.pack_into('<8I', image, phoff + i * phentsize, 1, offset,
                         address, address, len(content), len(content), 4, 0x100)
        image[offset:offset + len(content)] = content
    if duplicate:
        address, content = segments[0]
        second_offset = len(image)
        image.extend(content)
        struct.pack_into('<H', image, 44, len(segments)+1)
        struct.pack_into('<8I', image, phoff + len(segments) * phentsize,
                         1, second_offset, address, address, len(content),
                         len(content), 4, 0x100)
    return bytes(image)

class NameEntryImporterTests(unittest.TestCase):
    def fixture(self, segments=None):
        elf = synthetic_elf(segments)
        source = m.extract_source(m.ElfImage(elf))[0]
        m.ELF_SHA256 = hashlib.sha256(elf).hexdigest()
        m.SOURCE_SHA256 = hashlib.sha256(source).hexdigest()
        return elf, source, *m.import_elf(elf)

    def test_pointer_free_row_and_literals_layout(self):
        elf, source, output, metadata = self.fixture()
        expected_size = (m.HEADER.size + m.ROW_SIZE + 16 + 32 + 4 +
                         sum(4 + len(value) for value in STRING_SOURCES))
        self.assertEqual(len(output), expected_size)
        fields = m.HEADER.unpack_from(output)
        self.assertEqual(fields[:5], (b'OKNMED2\0', 2, 1, 28, 112))
        self.assertEqual(fields[5], hashlib.sha256(elf).digest())
        self.assertEqual(fields[6], hashlib.sha256(source).digest())

        at = m.HEADER.size
        imported = m.ROW.unpack_from(output, at)
        for index, word in enumerate(SOURCE_WORDS):
            self.assertEqual(imported[index], m.IMPORTED_BINDINGS[index]
                             if index in m.IMPORTED_BINDINGS else word)
        at += m.ROW_SIZE
        self.assertEqual(output[at:at+16], ATLAS_SOURCE); at += 16
        self.assertEqual(output[at:at+32], DOUBLE_SOURCE); at += 32
        self.assertEqual(output[at:at+4], DEFAULT_NAME_SOURCE); at += 4
        for value in STRING_SOURCES:
            size, = m.STRING_LENGTH.unpack_from(output, at); at += 4
            self.assertEqual(size, len(value))
            self.assertEqual(output[at:at+size], value); at += size
        self.assertEqual(at, len(output))
        self.assertEqual(metadata['atlas']['entries_before_terminator'], 0)
        self.assertEqual(metadata['source_bytes'], 173)
        self.assertEqual(metadata['record']['type'], 9)
        self.assertEqual(metadata['record']['capacities'], [32, 16])

    def test_source_range_validation(self):
        segment_by_address = dict(source_segments())
        cases = [
            (m.ROW_ADDRESS, bytearray(ROW_SOURCE), 112, 'interface terminator'),
            (m.ROW_ADDRESS, bytearray(ROW_SOURCE), 0, 'binding'),
            (m.ROW_ADDRESS, bytearray(ROW_SOURCE), 96, 'type'),
            (m.ROW_ADDRESS, bytearray(ROW_SOURCE), 100, 'capacities'),
            (m.ROW_ADDRESS, bytearray(ROW_SOURCE), 108, 'reserved'),
            (m.ATLAS_ADDRESS, bytearray(ATLAS_SOURCE), 0, 'atlas first tuple'),
            (0x222a80, bytearray(static_source()), 0x222a80 - 0x222a80, 'Unterminated'),
        ]
        writes = [
            lambda b: b.__setitem__(112, 1),
            lambda b: struct.pack_into('<I', b, 0, 0x1f897c),
            lambda b: struct.pack_into('<I', b, 96, 8),
            lambda b: struct.pack_into('<I', b, 100, 31),
            lambda b: struct.pack_into('<I', b, 108, 1),
            lambda b: b.__setitem__(0, 1),
            lambda b: b.__setitem__(slice(0, 64), b'x'*64),
        ]
        for (address, original, offset, message), mutate in zip(cases, writes):
            with self.subTest(message=message):
                mutate(original)
                segments = list(source_segments())
                index = next(i for i,(a,_) in enumerate(segments)
                             if a <= address < a + len(_))
                base, _ = segments[index]
                segments[index] = (base, bytes(original))
                image = m.ElfImage(synthetic_elf(segments))
                with self.assertRaisesRegex(ValueError, message):
                    m.extract_source(image)

    def test_hash_mismatch_and_elf_range_mapping(self):
        elf, source, _, _ = self.fixture()
        image = m.ElfImage(elf)
        self.assertEqual(image.read(m.ROW_ADDRESS, m.TERMINATED_ROW_SIZE), ROW_SOURCE)
        with self.assertRaisesRegex(ValueError, 'Unmapped or ambiguous'):
            image.read(m.ROW_ADDRESS - 1, 4)
        with self.assertRaisesRegex(ValueError, 'Unmapped or ambiguous'):
            image.read(m.ROW_ADDRESS, m.TERMINATED_ROW_SIZE + 1)
        duplicate = m.ElfImage(synthetic_elf(duplicate=True))
        with self.assertRaisesRegex(ValueError, 'Unmapped or ambiguous'):
            duplicate.read(m.ROW_ADDRESS, m.TERMINATED_ROW_SIZE)

        m.SOURCE_SHA256 = '0' * 64
        with self.assertRaisesRegex(ValueError, 'source literals SHA256'):
            m.import_elf(elf)
        m.ELF_SHA256 = '0' * 64
        with self.assertRaisesRegex(ValueError, 'ELF SHA256'):
            m.import_elf(elf)

    def test_output_writer_refuses_existing_and_symlink_targets(self):
        _, _, output, metadata = self.fixture()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target, manifest = root / 'name-entry.bin', root / 'manifest.json'
            m.write_outputs(target, manifest, output, metadata)
            self.assertEqual(target.read_bytes(), output)
            self.assertEqual(json.loads(manifest.read_text())['source_sha256'],
                             metadata['source_sha256'])
            with self.assertRaisesRegex(ValueError, 'existing output'):
                m.write_outputs(target, manifest, output, metadata)
            alias = root / 'alias'
            alias.symlink_to(target)
            with self.assertRaisesRegex(ValueError, 'symlink'):
                m.write_outputs(alias, root / 'other.json', output, metadata)

    def test_cli_rejects_unowned_input_without_writing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, output = root / 'not.elf', root / 'name-entry.bin'
            source.write_bytes(b'not an owned ELF')
            result = subprocess.run(
                [sys.executable, str(ROOT / 'tools/import_name_entry.py'),
                 '--elf', str(source), '--output', str(output)],
                capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertFalse(output.exists())
            self.assertFalse(output.with_name(output.name + '.manifest.json').exists())

if __name__ == '__main__':
    unittest.main()
