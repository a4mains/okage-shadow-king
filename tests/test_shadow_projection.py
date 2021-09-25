import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from verify_shadow_projection import compare, fixture, packet

def packed(words):
    return struct.pack('<%dI' % len(words), *words)

def wrap(body):
    body = list(body)
    body += [0]*(-len(body) % 4)
    size = (len(body)+8)*4
    return packed([0x10000000|len(body)//4,0,0,0x1800000+size]+body+[0x60000000,0,0,0])

COLOR = [0x11000000,0x50000002,0x8001,0x10000000,0xe,0,0,0x3f800000,1,0]
FIRST = [0x6c048000]+list(range(16))+[0x14000006]
SECOND = [0x6c048000]+list(range(16,32))+[0x14000008]
DIRECTION = [0x6c018000,0,1,2,0,0x1400000a]

class ShadowProjectionTests(unittest.TestCase):
    def test_packet_order_and_bounds(self):
        self.assertEqual(packet(wrap(COLOR)), ((0,0,0), [], None))
        color, matrices, direction = packet(wrap(COLOR+DIRECTION+FIRST+SECOND))
        self.assertEqual(matrices, [tuple(range(16)), tuple(range(16,32))])
        self.assertEqual(direction, (0,1,2,0))
        for body in (COLOR+FIRST, COLOR+SECOND+FIRST, COLOR+FIRST+SECOND+SECOND,
                     COLOR+DIRECTION[:-1], COLOR+FIRST+SECOND+DIRECTION,
                     COLOR+[0]*6, COLOR+[1]):
            with self.subTest(body=body):
                with self.assertRaises(ValueError):packet(wrap(body))
        good = wrap(COLOR+FIRST+SECOND)
        # Every DMA/GIF command and all color-only padding must be checked.
        for word in list(range(10))+[11,12,13,len(good)//4-4]:
            changed = bytearray(good);changed[word*4] ^= 1
            with self.subTest(word=word):
                with self.assertRaises(ValueError):packet(changed)
        bad = bytearray(wrap(COLOR));bad[4*(4+6)+3] = 1
        with self.assertRaises(ValueError):packet(bad)
        for data in (b'', good[:-4], good+b'\0'*16):
            with self.assertRaises(ValueError):packet(data)

    def test_comparison_checks_every_word_and_fixture(self):
        inputs = fixture()[:108]
        pkt = wrap(COLOR)
        original = packed([0,0,len(pkt)])+pkt
        native = packed([0]*43)
        setup = packed([0]*57)
        self.assertEqual(compare(original,native,setup,setup,inputs)['cases'],1)
        for word in range(43):
            changed = bytearray(native);changed[word*4] ^= 1
            with self.assertRaises(ValueError):compare(original,changed,setup,setup,inputs)
        for word in range(57):
            changed = bytearray(setup);changed[word*4] ^= 1
            with self.assertRaises(ValueError):compare(original,native,setup,changed,inputs)
        for bad in (b'',inputs[:-4],inputs*129,inputs*2):
            with self.assertRaises(ValueError):compare(original,native,setup,setup,bad)
        for index,value in ((0,1),(1,8),(2,0x7fc00000),(24,256)):
            bad = bytearray(inputs);struct.pack_into('<I',bad,index*4,value)
            with self.assertRaises(ValueError):compare(original,native,setup,setup,bad)
        for bad in (original[:-4],original+original):
            with self.assertRaises(ValueError):compare(bad,native,setup,setup,inputs)
        # Matching truncated outputs must not silently narrow the supplied corpus.
        with self.assertRaisesRegex(ValueError,'count'):
            compare(original,native,setup,setup,fixture()[:216])

    def test_mode_factor_and_tint_contract(self):
        inputs = bytearray(fixture()[:108])
        struct.pack_into('<I',inputs,4,4)
        setup = packed([0]*57)
        def run(body, direction):
            pkt = wrap(body)
            native = packed([0,4,1,int(direction),0,0,0]+list(range(32))+([0,1,2,0] if direction else [0]*4))
            return compare(packed([0,4,len(pkt)])+pkt,native,setup,setup,inputs)
        self.assertEqual(run(COLOR+FIRST+SECOND,False)['matrices_exact'],2)
        with self.assertRaisesRegex(ValueError,'mode/command'):run(COLOR+DIRECTION+FIRST+SECOND,True)
        struct.pack_into('<f',inputs,12,.5)
        self.assertEqual(run(COLOR+DIRECTION+FIRST+SECOND,True)['directions_exact'],1)
        with self.assertRaisesRegex(ValueError,'mode/command'):run(COLOR+FIRST+SECOND,False)
        bad = COLOR.copy();bad[6] = 1
        with self.assertRaisesRegex(ValueError,'tint'):run(bad+DIRECTION+FIRST+SECOND,True)
