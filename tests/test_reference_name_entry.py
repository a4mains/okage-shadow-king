"""Read-only name-entry writer tests with synthetic EE storage, no game execution."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STUB = r'''
#pragma once
#include <cstdint>
#include <vector>
using uint32=std::uint32_t;
namespace PS2 {constexpr unsigned EE_RAM_SIZE=32*1024*1024;}
struct CMIPS {enum {V0=2,A0=4,S1=17,SP=29,RA=31};};
namespace Ee {
struct State {uint32 nPC=0,nFCSR=0;struct {std::uint64_t nD0=0;} nGPR[32];};
struct CSubSystem {struct {State m_State;} m_EE;
std::vector<std::uint8_t> storage=std::vector<std::uint8_t>(PS2::EE_RAM_SIZE);
std::uint8_t* m_ram=storage.data();};}
'''
HARNESS = r'''
#include "reference_name_entry.hpp"
#include <iostream>
int main(int argc,char** argv){try {
 Ee::CSubSystem ee;auto put=[&](uint32 at,uint32 v){std::memcpy(ee.m_ram+at,&v,4);};
 constexpr uint32 manager=0x400000,owner=0x410000,parent=0x420000,window=0x430000,sp=0x700000;
 put(0x4014,2);put(manager,0x11fec8);put(manager+4,0x120ad0);put(manager+8,0x120b48);
 put(manager+0x14,owner);put(manager+0xe40,1);put(manager+0xa0,9);put(manager+0x9c,0x119700);
 put(manager+0x90,0x50);put(manager+0x114,manager);put(manager+0xe4c,0xffffffff);put(manager+0xa4,0xfffffffd);
 put(owner+0x14,parent);put(owner+0x24,9);put(owner+0x34,manager);put(parent+0x4c,9);
 put(0x200688,window);put(window+0x270,0x43000000);put(0x24f788,0x440000);
 put(window+0x3c00,32);put(window+0x3c04,16);
 put(window+0x650,0x460000);put(window+0x654,0x470000);
 put(window+0x658,0x480000);put(window+0x65c,0x490000);
 put(0x440844,0x00697241);put(0x33958c,0x00400010);put(0x339590,0x00800020);
 put(0x2151b4,0x450000);put(0x450058,0x12345678);put(0x24eb98,0x00000041);
 const std::string mode=argv[1];NameEntryTrace trace(argv[2]);
 auto stop=[&](uint32 pc,uint32 arg,uint32 stack,uint32 ra){auto& c=ee.m_EE.m_State;
  c.nPC=pc;c.nGPR[CMIPS::A0].nD0=arg;c.nGPR[CMIPS::SP].nD0=stack;c.nGPR[CMIPS::RA].nD0=ra;c.nGPR[CMIPS::S1].nD0=manager;
  const auto before=ee.storage;trace.observe(ee,7);if(before!=ee.storage)throw std::runtime_error("guest bytes mutated");};
 stop(NameEntryTrace::constructor,owner,sp,0x114000);
 if(mode=="bad-owner")put(owner+0x34,manager+16);
 if(mode=="bad-vars")put(0x24f788,PS2::EE_RAM_SIZE-4);
 if(mode=="bad-window")put(manager+0x40,8);
 stop(NameEntryTrace::first,manager,sp,NameEntryTrace::firstReturn);
 if(mode=="partial"){trace.finish();return 0;}
 put(sp-0xd0+0x10,0x42);put(sp-0xd0+0x14,0x24);put(sp-0xd0+0x1c,0x81);
 ee.m_EE.m_State.nGPR[CMIPS::V0].nD0=0x18;
 if(mode!="missing-pad") {
  stop(NameEntryTrace::padRead,0,sp-0xd0,0x11a230);
  if(mode=="pad-order")stop(NameEntryTrace::padRead,0,sp-0xd0,0x11a230);
  put(sp-0xd0+0x10,0x40);put(sp-0xd0+0x14,0x20);put(sp-0xd0+0x18,0x10);put(sp-0xd0+0x1c,0x80);
  stop(NameEntryTrace::padNormalized,0,sp-0xd0,0x11a274);
 }
 put(manager+0xe8c,1);put(manager+0xe4c,0xfffffffe);
 if(mode=="thread")put(0x4014,3);
 stop(NameEntryTrace::firstReturn,0,mode=="stack"?sp-16:sp,NameEntryTrace::firstReturn);
 if(mode=="thread"){trace.finish();return 0;}
 stop(NameEntryTrace::second,manager+0x40,sp,NameEntryTrace::secondReturn);
 if(mode=="bad-capacity")put(window+0x3c00,33);
 if(mode=="bad-buffer")put(window+0x650,PS2::EE_RAM_SIZE-4);
 stop(NameEntryTrace::secondReturn,0,sp,NameEntryTrace::secondReturn);
 trace.finish();return 0;
}catch(const std::exception& e){std::cerr<<e.what();return 1;}}
'''

@unittest.skipUnless(shutil.which('clang++'), 'Synthetic observer test requires clang++')
class NameEntryObserverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.base = Path(cls.temp.name)
        (cls.base / 'ee').mkdir()
        (cls.base / 'ee/Ee_SubSystem.h').write_text(STUB)
        (cls.base / 'test.cpp').write_text(HARNESS)
        cls.binary = cls.base / 'test'
        subprocess.run(['clang++', '-std=c++20', '-I'+str(cls.base),
                        '-I'+str(ROOT/'tools/play-oracle'), str(cls.base/'test.cpp'),
                        '-o', str(cls.binary)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def fixture(self, mode, path=None):
        path = path or self.base/(self.id().split('.')[-1]+'-'+mode+'.jsonl')
        run = subprocess.run([str(self.binary), mode, str(path)], capture_output=True, text=True)
        return run, path

    def test_paired_states_and_actual_consumed_pad_not_entry_latches(self):
        run, path = self.fixture('normal')
        self.assertEqual(run.returncode, 0, run.stderr)
        rows = list(map(json.loads, path.read_text().splitlines()))
        self.assertEqual(rows[0]['schema'], 'okage-name-entry-v1')
        self.assertEqual([r['edge'] for r in rows[1:-1]],
                         ['constructor_entry', 'first_entry', 'pad_read', 'pad_normalized',
                          'first_return', 'second_entry', 'second_return'])
        self.assertEqual(rows[2]['pressed_raw'], 0x40)
        self.assertEqual(rows[3]['consumed_pad'],
                         dict(pressed=0x42, released=0x24, held=0x18, repeat=0x81, normalized=False))
        self.assertEqual(rows[4]['consumed_pad']['pressed'], 0x40)
        self.assertTrue(rows[4]['consumed_pad']['normalized'])
        self.assertEqual(rows[2]['state_signed'], -1)
        self.assertEqual(rows[5]['state_signed'], -2)
        self.assertEqual(rows[5]['state_cursor'], 1)
        self.assertEqual(rows[2]['manager_raw']['size'], 0xef0)
        self.assertEqual(bytes.fromhex(rows[2]['name_bytes']['hex']), b'Ari\0\0\0\0\0')
        self.assertEqual(rows[-1]['first_pairs'], 1)
        self.assertEqual(rows[-1]['second_pairs'], 1)
        self.assertEqual(rows[-2]['window_raw']['size'], 0x6ec0)
        self.assertEqual(rows[-2]['glyph_positions']['size'], 33*256)
        self.assertEqual(rows[-2]['sprite_metrics']['size'], 17*256)
        self.assertEqual([r['sequence'] for r in rows[1:-1]], list(range(7)))

    def test_identity_extents_pairing_and_missing_observations_fail(self):
        for mode, error in [('bad-owner', 'identity'), ('bad-vars', 'outside RAM'),
                            ('bad-window', 'registry handle'), ('stack', 'PC/SP/RA'),
                            ('partial', 'incomplete'), ('thread', 'incomplete'),
                            ('missing-pad', 'missing consumed'), ('pad-order', 'continuation order'),
                            ('bad-capacity', 'capacities'), ('bad-buffer', 'outside RAM')]:
            with self.subTest(mode=mode):
                run, _ = self.fixture(mode)
                self.assertNotEqual(run.returncode, 0)
                self.assertIn(error, run.stderr)

    def test_existing_and_dangling_symlink_outputs_are_preserved(self):
        existing = self.base/'preserve.jsonl'
        existing.write_text('retained negative\n')
        run, _ = self.fixture('normal', existing)
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(existing.read_text(), 'retained negative\n')
        link = self.base/'dangling.jsonl'
        target = self.base/'missing.jsonl'
        link.symlink_to(target)
        run, _ = self.fixture('normal', link)
        self.assertNotEqual(run.returncode, 0)
        self.assertFalse(target.exists())

if __name__ == '__main__':
    unittest.main()
