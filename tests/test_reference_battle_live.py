"""Compile the real read-only observer against a synthetic EE memory surface.
No emulator, owned game asset, or captured state is used by these tests.
"""
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
using uint32=std::uint32_t;using uint16=std::uint16_t;
namespace PS2 {constexpr unsigned EE_RAM_SIZE=32*1024*1024;}
struct CMIPS {enum {A0=4,SP=29,RA=31};};
namespace Ee {
struct Gpr {std::uint64_t nD0=0;};
struct State {uint32 nPC=0,nFCSR=0;Gpr nGPR[32];};
struct Cpu {State m_State;std::vector<uint32> toggles;void ToggleBreakpoint(uint32 at){toggles.push_back(at);}};
struct CSubSystem {std::vector<std::uint8_t> backing=std::vector<std::uint8_t>(PS2::EE_RAM_SIZE);std::uint8_t* m_ram=backing.data();Cpu m_EE;};
}
'''
HARNESS = r'''
#include "reference_battle_live.hpp"
#include <iostream>
struct Fixture {
 Ee::CSubSystem ee;std::vector<uint32> bps;static constexpr uint32 root=0x400000,party=0x410000,enemy=0x420000,task=0x500000,ra=0x114abc,sp=0x700000;
 void put(uint32 at,uint32 v,unsigned n=4){for(unsigned i=0;i<n;++i)ee.m_ram[at+i]=v>>(8*i);}
 Fixture(){
  bps.assign(BattleLiveTrace::entryBreakpoints.begin(),BattleLiveTrace::entryBreakpoints.end());
  put(0x4014,2);put(0x1fce60,root);put(root,0x14a248);put(root+0x24,1);put(root+0x50,1);put(root+0x18,party);
  put(root+0x806c,party);put(root+0x8070,enemy);put(party,0x1524e0);put(party+0x14,root);put(party+0x10,enemy);put(party+0x18,task);put(party+0x38,1);put(party+0x3c,task);
  put(enemy,0x15afa0);put(enemy+0x14,root);put(enemy+0xc,party);put(task+0x14,party);put(root+0x60+0x1f4,task+0x40);
  put(task,0x153a20);put(task+4,0x153ce8);put(task+0x50,0x460000);put(task+0x130,0,2);
  put(0x24f79c,0x430000);put(0x24f788,0x440000);put(0x2151b4,0x450000);put(0x450058,0x12345678);
  put(0x430030,0x55aa,2);put(0x440574,0xdeadbeef);put(0x33958e,0x40,2);
  const unsigned offsets[]={8,0,0x4a,1,0x4c,0x4e};for(unsigned j=0;j<6;++j)put(task+0x14c+j*4,0x460000+offsets[j]);
  put(0x460008,999);put(0x460000,1,1);put(0x46004a,3,2);
  put(0x21301c,0x470000);put(0x213014,0x480000);put(0x33760c,0x490000);put(0x47000a,0xffff,2);put(0x480006,4,2);
 }
 void stop(uint32 pc,uint32 owner){auto& s=ee.m_EE.m_State;s.nPC=pc;s.nGPR[CMIPS::A0].nD0=owner;s.nGPR[CMIPS::SP].nD0=sp;s.nGPR[CMIPS::RA].nD0=ra;s.nFCSR=0x1000001;}
};
int main(int argc,char** argv){
 try {
  const std::string mode=argv[1];const std::filesystem::path path=argv[2];Fixture f;
  if(mode=="oversize"){BattleLiveTrace t(path,4097);return 0;}
  BattleLiveTrace t(path,8);
  if(mode=="empty"){t.finish(8);return 0;}
  if(mode=="count")f.put(Fixture::root+0x50,65);
  if(mode=="three-channels")f.put(0x480006,3,2);
  if(mode=="short-channels")f.put(0x480006,2,2);
  if(mode=="cycle")f.put(Fixture::party+0x10,Fixture::party);
  if(mode=="bad-actor")f.put(Fixture::task+0x130,7,2);
  if(mode=="shared-return")f.bps.push_back(Fixture::ra);
  f.stop(0x14a248,Fixture::root);t.observe(f.ee,3,f.bps);
  if(mode=="partial"){t.finish(8);return 0;}
  if(mode=="nested"){f.stop(0x1524e0,Fixture::party);t.observe(f.ee,3,f.bps);return 0;}
  f.put(Fixture::root,mode=="unknown-next"?0x123456:0x14a508);f.put(Fixture::root+0x24,0);f.put(0x450058,0x98765432);
  f.stop(Fixture::ra,0);if(mode=="thread")f.put(0x4014,3);if(mode=="stack")f.ee.m_EE.m_State.nGPR[CMIPS::SP].nD0-=16;
  if(mode=="root")f.put(0x1fce60,0x410000);
  t.observe(f.ee,4,f.bps);
  if(mode=="backwards"){f.stop(0x14a508,Fixture::root);t.observe(f.ee,2,f.bps);return 0;}
  for(auto pc:{0x14a508u,0x1519f8u,0x151e68u}) {
   f.put(Fixture::root,pc);f.stop(pc,Fixture::root);t.observe(f.ee,5,f.bps);f.stop(Fixture::ra,0);t.observe(f.ee,5,f.bps);
  }
  for(auto pair:{std::pair{0x1524e0u,Fixture::party},std::pair{0x15afa0u,Fixture::enemy}}){f.stop(pair.first,pair.second);t.observe(f.ee,6,f.bps);f.stop(Fixture::ra,0);t.observe(f.ee,6,f.bps);}
  t.finish(mode=="short-run"?7:8);
  if(mode=="shared-return"&&!f.ee.m_EE.toggles.empty())throw std::runtime_error("Shared return breakpoint changed");
  if(mode!="shared-return"&&f.ee.m_EE.toggles.size()!=12)throw std::runtime_error("Return breakpoint ownership count");
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
'''

@unittest.skipUnless(shutil.which('clang++'), 'Synthetic C++ observer tests require clang++')
class BattleLiveObserverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.base = Path(cls.temp.name)
        (cls.base / 'ee').mkdir()
        (cls.base / 'ee/Ee_SubSystem.h').write_text(STUB)
        (cls.base / 'harness.cpp').write_text(HARNESS)
        cls.binary = cls.base / 'test'
        subprocess.run(['clang++', '-std=c++20', '-I' + str(cls.base),
                        '-I' + str(ROOT / 'tools/play-oracle'), str(cls.base / 'harness.cpp'),
                        '-o', str(cls.binary)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, mode):
        path = self.base / (self.id().split('.')[-1] + '-' + mode + '.jsonl')
        result = subprocess.run([str(self.binary), mode, str(path)], capture_output=True, text=True)
        rows = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
        return result, rows, path

    def test_live_phase_transitions_and_rosters(self):
        result, rows, _ = self.run_case('normal')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(rows[0]['schema'], 'okage-battle-live-v1')
        callbacks = rows[1:-1]
        self.assertEqual(len(callbacks), 12)
        self.assertEqual([r['entry_pc'] for r in callbacks[::2]],
                         [0x14a248, 0x14a508, 0x1519f8, 0x151e68, 0x1524e0, 0x15afa0])
        self.assertEqual(callbacks[0]['frame'], 3)
        self.assertEqual(callbacks[1]['frame'], 4)
        self.assertEqual(callbacks[0]['rng_state_word'], 0x12345678)
        self.assertEqual(callbacks[1]['rng_state_word'], 0x98765432)
        self.assertEqual(callbacks[0]['registered_entities']['bytes'], 0x200)
        actor = callbacks[0]['actors'][0]
        self.assertEqual(actor['body_pointer'], 0x500040)
        self.assertEqual(actor['stat_bindings'][0]['value']['raw_hex'], 'e7030000')
        self.assertEqual(actor['model']['world_channels_raw']['bytes'], 4 * 0x90)
        self.assertEqual(rows[-1]['callback_frame_counts'], [[3, 1], [4, 1], [5, 6], [6, 4]])
        self.assertEqual(rows[-1]['pairs'], 6)

    def test_shared_breakpoint_is_not_removed(self):
        result, _, _ = self.run_case('shared-return')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_channel_extent_is_owned(self):
        result, rows, _ = self.run_case('three-channels')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(rows[1]['actors'][0]['model']['captured_channels'], 3)
        result, _, _ = self.run_case('short-channels')
        self.assertIn('channel extent', result.stderr)
        self.assertNotEqual(result.returncode, 0)

    def test_pairing_and_ownership_fail_closed(self):
        for mode, expected in [('thread', 'thread/SP/RA/root'), ('stack', 'thread/SP/RA/root'),
                               ('root', 'thread/SP/RA/root'), ('nested', 'nested callback'),
                               ('bad-actor', 'identity'), ('cycle', 'scheduler'),
                               ('backwards', 'backwards'), ('count', 'exceeds64')]:
            with self.subTest(mode=mode):
                result, rows, _ = self.run_case(mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)
                self.assertFalse(any(r.get('event') == 'finish' for r in rows))

    def test_unknown_next_callback_retains_return(self):
        result, rows, _ = self.run_case('unknown-next')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unobserved callback', result.stderr)
        self.assertEqual(rows[-1]['edge'], 'return')

    def test_extent_and_completion(self):
        for mode, expected in [('partial', 'unpaired'), ('empty', 'no complete'),
                               ('short-run', 'did not complete'), ('oversize', 'frame bound')]:
            with self.subTest(mode=mode):
                result, _, _ = self.run_case(mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)

    def test_refuses_existing_output(self):
        result, _, path = self.run_case('normal')
        self.assertEqual(result.returncode, 0, result.stderr)
        original = path.read_bytes()
        result = subprocess.run([str(self.binary), 'normal', str(path)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('existing output', result.stderr)
        self.assertEqual(path.read_bytes(), original)

if __name__ == '__main__':
    unittest.main()
