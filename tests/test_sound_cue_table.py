import pathlib
import re
import struct
import sys
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
from import_battle_data import ELF_SHA256, ElfImage
import hashlib
ELF = ROOT/'local/extracted/SCUS_971.29'

def literals(name):
    text = (ROOT/'include/okage/sound_cue_table.hpp').read_text()
    body = re.search(name + r'\{([^}]*)\}', text).group(1)
    return [int(v.rstrip('u'), 0) for v in re.findall(r'-?0x[0-9a-f]+u?|-?\d+', body)]

@unittest.skipUnless(ELF.exists(), 'owned ELF absent')
class SoundCueTableTests(unittest.TestCase):
    def test_literals_match_owned_elf_and_padding(self):
        data = ELF.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), ELF_SHA256)
        image = ElfImage(data)
        self.assertEqual(literals('sound_cue_volumes'), list(struct.unpack('<83b', image.read(0x1fb388, 83))))
        self.assertEqual(literals('sound_cue_speed_bits'), list(struct.unpack('<83I', image.read(0x1fb3e0, 83*4))))
        self.assertEqual(image.read(0x1fb388+83, 5), bytes(5))
        self.assertEqual(struct.unpack('<f', image.read(0x1fb3e0+83*4, 4))[0], 0.0)

if __name__ == '__main__':
    unittest.main()
