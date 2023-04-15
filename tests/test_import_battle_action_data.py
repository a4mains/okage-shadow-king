import pathlib
import struct
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
import import_battle_action_data as m

def xdb():
    b = bytearray(48+26*4+15)
    for at, value in {0:0x424458,4:104,8:15,12:1,16:0xffffffff,40:26}.items():
        struct.pack_into('<I',b,at,value)
    b[152:] = b'waza\0name\0desc\0'
    for row in range(2):
        struct.pack_into('<I',b,48+row*52,5)
        struct.pack_into('<I',b,48+row*52+48,10)
    return b

class Image:
    def __init__(self):
        self.regions = {0x23dd98:struct.pack('<256h',*([97]*256)),
                        0x243c08:struct.pack('<256h',*([158]*256))}
        for start,end,_,_ in m.REGIONS:
            self.regions[start] = bytes((i%251 for i in range(end-start)))
    def read(self, start, size):
        value = self.regions[start]
        if size != len(value):
            raise ValueError('fixture read extent')
        return value

class ActionDataTests(unittest.TestCase):
    def test_all_maps_and_regions_preserved(self):
        image=Image();payload,meta=m.extract_payload(image,256)
        expected=image.regions[0x23dd98]+image.regions[0x243c08]
        expected+=b''.join(image.regions[start] for start,_,_,_ in m.REGIONS)
        self.assertEqual(payload,expected)
        self.assertEqual([r['count'] for r in meta['regions']],[98,159,15])
    def test_mapping_negative_and_adjacent_region_reject(self):
        for at,value in ((0x23dd98,-1),(0x23dd98,98),(0x243c08,159)):
            image=Image();image.regions[at]=struct.pack('<h',value)+image.regions[at][2:]
            with self.assertRaisesRegex(ValueError,'mapping'):
                m.extract_payload(image,256)
        with self.assertRaisesRegex(ValueError,'count'):
            m.extract_payload(Image(),257)
    def test_xdb_extent_strings_and_zero_based_count(self):
        b=xdb();self.assertEqual(m.waza_count(b),2)
        struct.pack_into('<I',b,48+48,15)
        with self.assertRaisesRegex(ValueError,'string'):
            m.waza_count(b)
        b=xdb();struct.pack_into('<I',b,40,25)
        with self.assertRaisesRegex(ValueError,'extent'):
            m.waza_count(b)
        with self.assertRaisesRegex(ValueError,'extent'):
            m.waza_count(xdb()[:-1])
    def test_sources_pinned_and_cli_fresh_outputs(self):
        with self.assertRaisesRegex(ValueError,'SHA256'):
            m.import_data(b'elf',b'xdb')
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d);(p/'elf').write_bytes(b'e');(p/'xdb').write_bytes(b'x')
            args=['--elf',str(p/'elf'),'--bkdata',str(p/'xdb'),'--output',str(p/'out')]
            with mock.patch.object(m,'import_data',return_value=(b'artifact',{'a':1})):
                self.assertEqual(m.main(args),0)
            self.assertEqual((p/'out').read_bytes(),b'artifact')
            with self.assertRaisesRegex(ValueError,'existing'):
                m.main(args)
            (p/'out').unlink();(p/'out').symlink_to(p/'missing')
            with self.assertRaisesRegex(ValueError,'symlink'):
                m.main(args)

if __name__=='__main__':unittest.main()
