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
SPEC = importlib.util.spec_from_file_location('import_restore_data', ROOT / 'tools/import_restore_data.py')
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)

SOURCE_WORDS = (
    0x24f6d0, 25, 20, 1, 0xc39f8000, 0xc29e0000, 0x43828cd9, 0x3f800000,
    0, 0xbfc00000, 0, 0, 0, 0x43100000, 0, 0, 0x53f0167d, 0x53f0167d,
    0x53780348, 0x53780348, 1, 0x1f8e98, 0x11b518, 0x11c2c8,
    8, 17, 16, 0,
)
ROW_SOURCE = struct.pack('<28I', *SOURCE_WORDS) + bytes(4)
ATLAS_SOURCE = bytes(16)
DISPATCH_SOURCE = struct.pack('<I', m.DISPATCH_HANDLER)
STRING_VALUES = tuple((b'\x81\x40\0' if index == 0 else f's{index + 1}'.encode() + b'\0')
                      for index, _ in enumerate(m.STRING_SPECS))

def source_segments():
    return ((m.ROW_ADDRESS, ROW_SOURCE),
            (m.ATLAS_ADDRESS, ATLAS_SOURCE),
            (m.DISPATCH_TABLE_ADDRESS + m.DISPATCH_SELECTOR * 4, DISPATCH_SOURCE),
            *((address, value) for (_, _, address), value in zip(m.STRING_SPECS, STRING_VALUES)))

def synthetic_elf(segments=None, duplicate=False):
    segments = list(source_segments() if segments is None else segments)
    phoff, phentsize = 52, 32
    data_offset = (phoff + len(segments) * phentsize + 15) & ~15
    offsets, total = [], data_offset
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
        offset = len(image)
        image.extend(content)
        struct.pack_into('<H', image, 44, len(segments) + 1)
        struct.pack_into('<8I', image, phoff + len(segments) * phentsize,
                         1, offset, address, address, len(content), len(content), 4, 0x100)
    return bytes(image)

class RestoreDataImporterTests(unittest.TestCase):
    def import_fixture(self, segments=None):
        elf = synthetic_elf(segments)
        source = m.extract_source(m.ElfImage(elf))[0]
        old_elf, old_source = m.ELF_SHA256, m.SOURCE_SHA256
        self.addCleanup(setattr, m, 'ELF_SHA256', old_elf)
        self.addCleanup(setattr, m, 'SOURCE_SHA256', old_source)
        m.ELF_SHA256 = hashlib.sha256(elf).hexdigest()
        m.SOURCE_SHA256 = hashlib.sha256(source).hexdigest()
        return elf, source, *m.import_elf(elf)

    def test_pointer_free_definition_atlas_stream_and_all_literals(self):
        elf, source, output, metadata = self.import_fixture()
        self.assertEqual(m.HEADER.size, 88)
        fields = m.HEADER.unpack_from(output)
        self.assertEqual(fields[:5], (b'OKRST1\0\0', 1, 28, 1, 44))
        self.assertEqual(fields[5], hashlib.sha256(elf).digest())
        self.assertEqual(fields[6], hashlib.sha256(source).digest())
        imported = m.ROW.unpack_from(output, m.HEADER.size)
        for index, word in enumerate(SOURCE_WORDS):
            self.assertEqual(imported[index], 1 if index in m.IMPORTED_BINDINGS else word)
        at = m.HEADER.size + m.ROW_SIZE
        self.assertEqual(output[at:at + 16], ATLAS_SOURCE)
        at += 16
        for (string_id, key, address), value in zip(m.STRING_SPECS, STRING_VALUES):
            got_id, length = m.STRING_HEADER.unpack_from(output, at)
            self.assertEqual((got_id, length), (string_id, len(value)))
            self.assertEqual(output[at + 8:at + 8 + length], value)
            at += 8 + length
        self.assertEqual(at, len(output))
        self.assertEqual(metadata['record']['type'], 8)
        self.assertEqual(metadata['record']['capacities'], [17, 16])
        self.assertEqual(metadata['stream_initialization'][0]['opcode'], 14)
        self.assertEqual(metadata['stream_initialization'][1]['opcode'], 6)
        self.assertEqual(metadata['strings'][0]['key'], 'Slash')
        self.assertEqual(metadata['strings'][0]['bytes_hex'], '814000')

    def test_rejects_bad_row_terminator_bindings_shape_atlas_or_dispatch(self):
        cases = [
            (m.ROW_ADDRESS, ROW_SOURCE, lambda b: b.__setitem__(112, 1), 'row terminator'),
            (m.ROW_ADDRESS, ROW_SOURCE, lambda b: struct.pack_into('<I', b, 0, 0x24f6d4), 'binding'),
            (m.ROW_ADDRESS, ROW_SOURCE, lambda b: struct.pack_into('<I', b, 96, 9), 'row type'),
            (m.ROW_ADDRESS, ROW_SOURCE, lambda b: struct.pack_into('<I', b, 100, 16), 'capacities'),
            (m.ROW_ADDRESS, ROW_SOURCE, lambda b: struct.pack_into('<I', b, 108, 1), 'reserved'),
            (m.ATLAS_ADDRESS, ATLAS_SOURCE, lambda b: b.__setitem__(0, 1), 'entries before terminator'),
            (m.DISPATCH_TABLE_ADDRESS + m.DISPATCH_SELECTOR * 4, DISPATCH_SOURCE,
             lambda b: struct.pack_into('<I', b, 0, 0x11e484), 'dispatch handler'),
        ]
        base = list(source_segments())
        for address, original, mutate, message in cases:
            with self.subTest(message=message):
                changed = bytearray(original)
                mutate(changed)
                segments = list(base)
                index = next(i for i, (base_address, content) in enumerate(segments)
                             if base_address == address)
                segments[index] = (address, bytes(changed))
                if address == m.ATLAS_ADDRESS:
                    segments[index] = (address, bytes(changed) + bytes(m.ATLAS_TUPLE_SIZE))
                with self.assertRaisesRegex(ValueError, message):
                    m.extract_source(m.ElfImage(synthetic_elf(segments)))

    def test_rejects_unterminated_string_and_unmapped_or_ambiguous_ranges(self):
        segments = list(source_segments())
        address = m.STRING_SPECS[-1][2]
        index = next(i for i, (base, _) in enumerate(segments) if base == address)
        segments[index] = (address, b'x' * m.STRING_MAX_SIZE)
        with self.assertRaisesRegex(ValueError, 'Unterminated Restore string'):
            m.extract_source(m.ElfImage(synthetic_elf(segments)))
        image = m.ElfImage(synthetic_elf())
        with self.assertRaisesRegex(ValueError, 'Unmapped or ambiguous'):
            image.read(m.ROW_ADDRESS - 1, 4)
        with self.assertRaisesRegex(ValueError, 'Unmapped or ambiguous'):
            m.ElfImage(synthetic_elf(duplicate=True)).read(m.ROW_ADDRESS, m.ROW_TERMINATED_SIZE)

    def test_hash_pin_and_source_digest_fail_closed(self):
        elf, _, _, _ = self.import_fixture()
        source_hash = m.SOURCE_SHA256
        m.SOURCE_SHA256 = '0' * 64
        with self.assertRaisesRegex(ValueError, 'source literals SHA256'):
            m.import_elf(elf)
        m.SOURCE_SHA256 = source_hash
        m.ELF_SHA256 = '0' * 64
        with self.assertRaisesRegex(ValueError, 'ELF SHA256'):
            m.import_elf(elf)

    def test_output_writer_refuses_existing_and_symlink_targets(self):
        _, _, output, metadata = self.import_fixture()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target, manifest = root / 'restore.bin', root / 'manifest.json'
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
            elf, output = root / 'not.elf', root / 'restore.bin'
            elf.write_bytes(b'not an owned ELF')
            result = subprocess.run(
                [sys.executable, str(ROOT / 'tools/import_restore_data.py'),
                 '--elf', str(elf), '--output', str(output)],
                capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertFalse(output.exists())
            self.assertFalse(output.with_name(output.name + '.manifest.json').exists())

if __name__ == '__main__':
    unittest.main()
