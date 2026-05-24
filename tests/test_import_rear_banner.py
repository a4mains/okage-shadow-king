import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
SPEC = importlib.util.spec_from_file_location('import_rear_banner', ROOT / 'tools/import_rear_banner.py')
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)

SOURCE_WORDS = (
    0x1f86c0, 25, 40, 1, 0xc39f8000, 0xc29e0000, 0x43828cd9, 0x3f800000,
    0, 0xbfc00000, 0, 0, 0, 0x43100000, 0, 0, 0x53f0167d, 0x53f0167d,
    0x53780348, 0x53780348, 1, 0x1f8610, 0x115b20, 0x115bd8,
    13, 8, 2, 0,
)

def source_segments(words=SOURCE_WORDS, stream=bytes(m.STREAM_BYTES), atlas=bytes(16),
                    dispatch=m.DISPATCH_HANDLER, argument=m.ARGUMENT):
    return ((m.ROW_ADDRESS, struct.pack('<28I', *words) + bytes(4)),
            (m.STREAM_ADDRESS, stream),
            (m.ATLAS_ADDRESS, atlas),
            (m.DISPATCH_TABLE_ADDRESS + m.DISPATCH_SELECTOR * 4, struct.pack('<I', dispatch)),
            (m.ARGUMENT_ADDRESS, argument))

def synthetic_elf(segments):
    segments = list(segments)
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
    return bytes(image)

class RearBannerImporterTests(unittest.TestCase):
    def import_fixture(self, segments=None):
        elf = synthetic_elf(source_segments() if segments is None else segments)
        source = m.extract_source(m.ElfImage(elf))[0]
        old_elf, old_source = m.ELF_SHA256, m.SOURCE_SHA256
        self.addCleanup(setattr, m, 'ELF_SHA256', old_elf)
        self.addCleanup(setattr, m, 'SOURCE_SHA256', old_source)
        m.ELF_SHA256 = hashlib.sha256(elf).hexdigest()
        m.SOURCE_SHA256 = hashlib.sha256(source).hexdigest()
        return elf, source, *m.import_elf(elf)

    def test_pointer_free_row_atlas_and_argument(self):
        elf, source, output, metadata = self.import_fixture()
        fields = m.HEADER.unpack_from(output)
        self.assertEqual(fields[:5], (b'OKRBN1\0\0', 1, 28, 1, 16))
        self.assertEqual(fields[5], hashlib.sha256(elf).digest())
        self.assertEqual(fields[6], hashlib.sha256(source).digest())
        words = m.ROW.unpack_from(output, m.HEADER.size)
        expected = list(SOURCE_WORDS)
        for index in (0, 21, 22, 23):
            expected[index] = 1
        self.assertEqual(words, tuple(expected))
        tail = output[m.HEADER.size + m.ROW.size:]
        self.assertEqual(tail, bytes(16) + b'surprise attack\0')
        self.assertEqual(len(output), 232)
        self.assertEqual(metadata['output_sha256'], hashlib.sha256(output).hexdigest())
        self.assertEqual(metadata['record']['capacities'], [8, 2])

    def test_rejects_unsupported_source(self):
        bad_words = list(SOURCE_WORDS)
        bad_words[25] = 9
        cases = (
            source_segments(words=tuple(bad_words)),
            source_segments(stream=struct.pack('<5I', 14, 0x1f9c00, 6, 0x1f9c2c, 0)),
            source_segments(atlas=struct.pack('<4I', 1, 2, 3, 4)),
            source_segments(dispatch=0x115c40),
            source_segments(argument=b'surprise Attack\0'),
        )
        for segments in cases:
            with self.assertRaises(ValueError):
                m.extract_source(m.ElfImage(synthetic_elf(segments)))
        elf = synthetic_elf(source_segments())
        with self.assertRaises(ValueError):
            m.import_elf(elf)  # Retail ELF hash pin.

    def test_write_outputs_refuses_existing(self):
        _, _, output, metadata = self.import_fixture()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'rear-banner.bin'
            manifest = Path(directory) / 'rear-banner.bin.manifest.json'
            m.write_outputs(path, manifest, output, metadata)
            self.assertEqual(path.read_bytes(), output)
            self.assertEqual(json.loads(manifest.read_text())['schema'], 'okage-rear-banner-import-v1')
            with self.assertRaises(ValueError):
                m.write_outputs(path, manifest, output, metadata)

if __name__ == '__main__':
    unittest.main()
