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
SPEC = importlib.util.spec_from_file_location('import_results_ui', ROOT / 'tools/import_results_ui.py')
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)

ROW_SOURCE = bytes.fromhex(
    '30192e002c000000280000000100000000809fc300009ec2d98c82430000803f'
    '000000000000c0bf000000000000000000000000000010430000000000000000'
    '7d16f0537d16f053480378534803785301000000d8dd1f0000991600c8b61600'
    '0b00000021000000110000000000000000000000')
WIDGET_SOURCE = struct.pack('<16I', *m.WIDGET_IDS)
ANIMATION_SOURCE = b''.join(struct.pack('<4I', *record) for record in (
    (0,1,591,0),(4,0,0,0),(5,0,0,0),(6,0,0,0),(7,0,0,0),(1,1,591,0),
    (8,0,0,0),(55,0,0,0),(10,0,0,0),(11,0,0,0),(62,0,0,0),(12,0,0,0),
    (63,0,0,0),(0xffffffff,0xffffffff,0,0),
    (2,1,591,0),(56,1,573,0),(13,0,0,0),(14,0,0,0),(64,2,0,0),(110,0,0,0),
    (65,2,0,0),(111,0,0,0),(66,2,0,0),(112,0,0,0),(67,2,0,0),(113,0,0,0),
    (114,2,0,0),(118,0,0,0),(115,2,0,0),(119,0,0,0),(116,2,0,0),(120,0,0,0),
    (117,2,0,0),(121,0,0,0),(15,0,0,0),(16,0,0,0),(68,0,0,0),
    (0xffffffff,0xffffffff,0,0),
    (17,0,0,0),(57,1,129,0),(18,0,0,0),(19,0,0,0),(69,0,0,0),
    (0xffffffff,0xffffffff,0,0),
    (3,1,591,0),(58,1,564,0),(20,0,0,0),(21,0,0,0),
    (0xffffffff,0xffffffff,0,0)))
STREAM_SOURCE = (struct.pack('<4I', *m.STREAM_CONSTANT_VALUES[0]) +
                 struct.pack('<4I', *m.STREAM_CONSTANT_VALUES[1]))
SEGMENTS = ((m.ROW_ADDRESS, ROW_SOURCE),
            (m.WIDGET_ADDRESS, WIDGET_SOURCE),
            (m.ANIMATION_ADDRESS, ANIMATION_SOURCE),
            (m.STREAM_CONSTANT_SPANS[0][0], STREAM_SOURCE[:16]),
            (m.STREAM_CONSTANT_SPANS[1][0], STREAM_SOURCE[16:]))

def synthetic_elf(segments=SEGMENTS):
    phoff = 52
    phentsize = 32
    data_offset = (phoff + len(segments) * phentsize + 15) & ~15
    offsets = []
    total = data_offset
    for _, data in segments:
        offsets.append(total)
        total += len(data)
    image = bytearray(total)
    image[:16] = b'\x7fELF\x01\x01\x01' + bytes(9)
    struct.pack_into('<HHIIIIIHHHHHH', image, 16, 2, 8, 1, 0, phoff,
                     0, 0, 52, phentsize, len(segments), 40, 0, 0)
    for i, ((address, data), offset) in enumerate(zip(segments, offsets)):
        struct.pack_into('<8I', image, phoff + i * phentsize, 1, offset,
                         address, address, len(data), len(data), 4, 0x100)
        image[offset:offset + len(data)] = data
    return bytes(image)

class ResultsUIImporterTests(unittest.TestCase):
    def setUp(self):
        m.ELF_SHA256 = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'
        m.SOURCE_SHA256 = '9307f0adb6f8183153095634ccf9ac284d2d9bc1ed320d13ea050099af7aa183'

    def import_fixture(self):
        elf = synthetic_elf()
        m.ELF_SHA256 = hashlib.sha256(elf).hexdigest()
        return elf, *m.import_elf(elf)

    def test_layout_binding_rewrite_and_preserved_row(self):
        elf, output, metadata = self.import_fixture()
        self.assertEqual(len(output), 88 + 112 + 64 + 49 * 16 + 32)
        fields = m.HEADER.unpack_from(output)
        self.assertEqual(fields[:5], (b'OKRSUI1\0', 1, 28, 16, 49))
        self.assertEqual(fields[5], hashlib.sha256(elf).digest())
        self.assertEqual(fields[6].hex(), m.SOURCE_SHA256)
        source_words = m.ROW.unpack(ROW_SOURCE[:m.ROW_SIZE])
        imported = m.ROW.unpack_from(output, 88)
        for index in range(28):
            self.assertEqual(imported[index], 1 if index in (0, 21, 22, 23) else source_words[index])
        self.assertEqual(output[200:264], WIDGET_SOURCE)
        self.assertEqual(output[264:264 + len(ANIMATION_SOURCE)], ANIMATION_SOURCE)
        self.assertEqual(output[-32:], STREAM_SOURCE)
        self.assertEqual(metadata['animation_table_record_counts'], [14, 24, 6, 5])

    def test_rejects_bad_terminators_bindings_tables_and_constants(self):
        bad_row = bytearray(ROW_SOURCE); bad_row[-1] = 1
        image = m.ElfImage(synthetic_elf(((m.ROW_ADDRESS, bytes(bad_row)), *SEGMENTS[1:])))
        with self.assertRaisesRegex(ValueError, 'row terminator'):
            m.extract_source(image)
        changed_row = bytearray(ROW_SOURCE); struct.pack_into('<I', changed_row, 0, 0x2e1934)
        image = m.ElfImage(synthetic_elf(((m.ROW_ADDRESS, bytes(changed_row)), *SEGMENTS[1:])))
        with self.assertRaisesRegex(ValueError, 'binding'):
            m.extract_source(image)
        bad_animation = bytearray(ANIMATION_SOURCE); struct.pack_into('<I', bad_animation, 13 * 16, 0)
        image = m.ElfImage(synthetic_elf((SEGMENTS[0], SEGMENTS[1], (m.ANIMATION_ADDRESS, bytes(bad_animation)), *SEGMENTS[3:])))
        with self.assertRaisesRegex(ValueError, 'terminator'):
            m.extract_source(image)
        bad_stream = bytearray(STREAM_SOURCE); struct.pack_into('<I', bad_stream, 0, 13)
        image = m.ElfImage(synthetic_elf((*SEGMENTS[:3], (m.STREAM_CONSTANT_SPANS[0][0], bytes(bad_stream[:16])),
                                         (m.STREAM_CONSTANT_SPANS[1][0], bytes(bad_stream[16:])))))
        with self.assertRaisesRegex(ValueError, 'stream constants'):
            m.extract_source(image)

    def test_rejects_unmapped_extent_and_wrong_source_hash(self):
        elf = synthetic_elf(SEGMENTS[:-1])
        m.ELF_SHA256 = hashlib.sha256(elf).hexdigest()
        with self.assertRaisesRegex(ValueError, 'Unmapped or ambiguous'):
            m.import_elf(elf)
        elf = synthetic_elf()
        m.ELF_SHA256 = hashlib.sha256(elf).hexdigest()
        m.SOURCE_SHA256 = '0' * 64
        with self.assertRaisesRegex(ValueError, 'source bytes SHA256'):
            m.import_elf(elf)
        m.SOURCE_SHA256 = '9307f0adb6f8183153095634ccf9ac284d2d9bc1ed320d13ea050099af7aa183'

    def test_hash_pin_and_fresh_output_refusal(self):
        elf, output, metadata = self.import_fixture()
        m.ELF_SHA256 = '0' * 64
        with self.assertRaisesRegex(ValueError, 'ELF SHA256'):
            m.import_elf(elf)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target, manifest = root / 'results-ui.bin', root / 'manifest.json'
            m.write_outputs(target, manifest, output, metadata)
            self.assertEqual(target.read_bytes(), output)
            self.assertEqual(json.loads(manifest.read_text())['source_sha256'], m.SOURCE_SHA256)
            with self.assertRaisesRegex(ValueError, 'existing output'):
                m.write_outputs(target, manifest, output, metadata)
            alias = root / 'alias'; alias.symlink_to(target)
            with self.assertRaisesRegex(ValueError, 'symlink'):
                m.write_outputs(alias, root / 'other.json', output, metadata)

if __name__ == '__main__':
    unittest.main()
