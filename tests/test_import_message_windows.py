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
SPEC = importlib.util.spec_from_file_location('import_message_windows', ROOT / 'tools/import_message_windows.py')
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)
sys.path.insert(0, str(ROOT / "tests"))
from test_import_rear_banner import synthetic_elf

ROW_A = (0x2dbb40, 0x20, 5, 1, 0xc39f8000, 0xc29e0000, 0x43828cd9, 0x3f800000, 0, 0xbfc00000,
         0, 0, 0, 0x43100000, 0, 0, 0x53f0167d, 0x53f0167d, 0x53780348, 0x53780348,
         0, 0x1fcdb0, 0x147ec8, 0x147fe0, 0, 16, 8, 0)
ROW_B = (0x2dbc40, 0x20, 3, 2, 0xc39f8000, 0xc2ce0000, 0x42c80000, 0x3f800000, 0, 0,
         0, 0, 0, 0x43100000, 0, 0, 0x53f0167d, 0x53f0167d, 0x53780348, 0x53780348,
         0, 0x1fcdb0, 0x1468c8, 0x146ee8, 1, 12, 2, 0)

def segments(row_b=ROW_B, terminator=bytes(4), selectors=(0x27, 0x28, 0x29, 0), space=b' \0'):
    rows = struct.pack('<28I', *ROW_A) + struct.pack('<28I', *row_b) + terminator
    return ((m.ROWS_ADDRESS, rows), (m.ATLAS_ADDRESS, bytes(16)),
            (m.DISPATCH_TABLE_ADDRESS, struct.pack('<2I', 0x148958, 0x148dc8)),
            (m.SELECTORS_ADDRESS, struct.pack('<4I', *selectors)),
            (m.PERIODS_ADDRESS, struct.pack('<4i', -4, 1, 2, 0)),
            (m.PERIOD_WORDS_ADDRESS, struct.pack('<5I', 0xfffffffe, 0xfffffffd, 0, 6, 0xfffffc18)),
            (0x228ff8, space + bytes(6) + b'NULL\0' + bytes(3) + b'system\0'))

class MessageWindowImporterTests(unittest.TestCase):
    def run_import(self, segs=None):
        elf = synthetic_elf(segments() if segs is None else segs)
        source = m.extract_source(m.ElfImage(elf))[0]
        old = (m.ELF_SHA256, m.SOURCE_SHA256)
        self.addCleanup(lambda: (setattr(m, 'ELF_SHA256', old[0]), setattr(m, 'SOURCE_SHA256', old[1])))
        m.ELF_SHA256 = hashlib.sha256(elf).hexdigest()
        m.SOURCE_SHA256 = hashlib.sha256(source).hexdigest()
        return m.import_elf(elf)

    def test_rows_are_mapped_to_logical_bindings(self):
        data, meta = self.run_import()
        self.assertEqual(len(data), 378)
        self.assertEqual(data[:8], b'OKMSW1\0\0')
        rows = [struct.unpack_from('<28I', data, 88 + i * 112) for i in range(2)]
        self.assertEqual((rows[0][0], rows[0][21], rows[0][22], rows[0][23], rows[0][24]), (1, 1, 1, 1, 0))
        self.assertEqual((rows[1][0], rows[1][21], rows[1][22], rows[1][23], rows[1][24]), (2, 1, 2, 2, 1))
        self.assertEqual(rows[0][4:20], ROW_A[4:20])
        self.assertEqual(data[-14:], b' \0NULL\0system\0')
        self.assertEqual(meta['output_sha256'], hashlib.sha256(data).hexdigest())

    def test_rejects_changed_literals(self):
        bad_row = list(ROW_B)
        bad_row[25] = 13
        for segs in (segments(row_b=tuple(bad_row)), segments(terminator=b'\1\0\0\0'),
                     segments(selectors=(0x27, 0x28, 0x2a, 0)), segments(space=b'x\0')):
            with self.assertRaises(ValueError):
                self.run_import(segs)

    def test_rejects_unsupported_elf_and_existing_output(self):
        with self.assertRaises(ValueError):
            m.import_elf(synthetic_elf(segments()))
        data, meta = self.run_import()
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / 'w.bin'
            m.write_outputs(out, Path(d) / 'w.json', data, meta)
            self.assertEqual(json.loads((Path(d) / 'w.json').read_text())['output_bytes'], 378)
            with self.assertRaises(ValueError):
                m.write_outputs(out, Path(d) / 'w2.json', data, meta)

if __name__ == '__main__':
    unittest.main()
