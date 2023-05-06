import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import import_battle_enemy_commands as m

class Image:
    def __init__(self):
        self.payload=bytes(i%251 for i in range(84*7*4))
        self.boundary=struct.pack('<10I',*m.BOUNDARY)
        self.reads=[]
    def read(self,address,size):
        self.reads.append((address,size))
        if (address,size)==(m.START,2352):return self.payload
        if (address,size)==(m.END,40):return self.boundary
        raise ValueError('Unexpected source extent')

class EnemyCommandsTests(unittest.TestCase):
    def test_complete_cells_and_independent_boundary(self):
        image=Image()
        self.assertEqual(m.extract_payload(image),image.payload)
        self.assertEqual(image.reads,[(0x2476c0,40),(0x246d90,2352)])
        image.boundary=image.boundary[:-1]+b'\xff'
        with self.assertRaisesRegex(ValueError,'switch table'):m.extract_payload(image)

    def test_payload_extent_not_inferred_or_padded(self):
        for size in [0,2351,2353]:
            image=Image();image.payload=bytes(size)
            with self.assertRaisesRegex(ValueError,'Truncated'):m.extract_payload(image)

    def test_hash_pin_precedes_elf_decoding(self):
        with mock.patch.object(m,'ElfImage') as reader:
            with self.assertRaisesRegex(ValueError,'SHA256'):m.import_data(b'not owned ELF')
            reader.assert_not_called()

    def test_header_and_manifest_bind_exact_payload(self):
        data=b'synthetic-source';image=Image()
        with mock.patch.object(m,'ELF_SHA256',hashlib.sha256(data).hexdigest()),mock.patch.object(m,'ElfImage',return_value=image):
            artifact,manifest=m.import_data(data)
        magic,version,rows,slots,width,digest=m.HEADER.unpack_from(artifact)
        self.assertEqual((magic,version,rows,slots,width),(b'OKBECMD1',1,84,7,4))
        self.assertEqual(digest,hashlib.sha256(data).digest())
        self.assertEqual(artifact[56:],image.payload)
        self.assertEqual(manifest['payload_sha256'],hashlib.sha256(image.payload).hexdigest())
        self.assertEqual(manifest['artifact_sha256'],hashlib.sha256(artifact).hexdigest())
        self.assertEqual(manifest['artifact_bytes'],2408)

    def test_fresh_outputs_and_path_collisions(self):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory);(p/'elf').write_bytes(b'e')
            args=['--elf',str(p/'elf'),'--output',str(p/'battle-enemy-commands.bin')]
            with mock.patch.object(m,'import_data',return_value=(b'artifact',{'schema':'fixture'})):
                self.assertEqual(m.main(args),0)
            self.assertEqual((p/'battle-enemy-commands.bin').read_bytes(),b'artifact')
            with self.assertRaisesRegex(ValueError,'existing'):m.main(args)
            (p/'battle-enemy-commands.bin').unlink()
            #A preexisting manifest alone must prevent even creating the artifact.
            with self.assertRaisesRegex(ValueError,'existing'):m.main(args)
            self.assertFalse((p/'battle-enemy-commands.bin').exists())
            (p/'battle-enemy-commands.bin.manifest.json').unlink()
            (p/'battle-enemy-commands.bin').symlink_to(p/'missing')
            with self.assertRaisesRegex(ValueError,'symlink'):m.main(args)
            (p/'battle-enemy-commands.bin').unlink()
            with self.assertRaisesRegex(ValueError,'differ'):m.main(args+['--manifest',str(p/'battle-enemy-commands.bin')])

if __name__=='__main__':unittest.main()
