"""Exercise the real snapshot writer with synthetic EE storage, no execution."""
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class ResultsObserverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang++') or shutil.which('c++')
        if not compiler:
            raise unittest.SkipTest('C++ compiler unavailable')
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = pathlib.Path(cls.temp.name)
        (cls.directory / 'ee').mkdir()
        (cls.directory / 'ee/Ee_SubSystem.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
using uint32=std::uint32_t;
namespace PS2 {constexpr unsigned EE_RAM_SIZE=32*1024*1024;}
struct CMIPS {enum {A0=4,RA=31};};
namespace Ee {
struct CSubSystem {
 struct {struct {unsigned nPC=0;struct {std::uint64_t nD0=0;} nGPR[32];} m_State;} m_EE;
 std::vector<unsigned char> storage=std::vector<unsigned char>(PS2::EE_RAM_SIZE);
 unsigned char* m_ram=storage.data();
};}
''')
        source = cls.directory / 'fixture.cpp'
        source.write_text(r'''
#include "reference_game_mode.hpp"
#include <iostream>
int main(int argc,char** argv) {try {
 Ee::CSubSystem ee;
 auto put=[&](unsigned p,unsigned v){std::memcpy(ee.m_ram+p,&v,4);};
 const unsigned parent=0x110000,field=0x111000,manager=0x112000,child=0x113000,storage=0x114000;
 put(parent+0x18,field);put(field,0);put(field+4,0x133a98);put(field+8,0x133a38);
 put(field+0x48,0x12);put(field+0x30,1);put(field+0x34,3);put(field+0x38,90);
 put(field+0x50,manager);put(manager,0x11fec8);put(manager+4,0x120ad0);
 put(manager+8,0x120b48);put(manager+0xe40,1);put(manager+0xa0,11);
 put(parent+0x38,child);put(child+4,0x12c8f8);put(child+8,0x12c8d8);
 put(child+0x24,3);put(child+0x30,17);put(child+0x3c,64);put(child+0x40,0x3f800000);put(child+0x44,0x223790);
 put(0x2597a8,2);put(0x2597ac,150);put(0x2597b0,0x400);put(0x2597ca,0x5e4);
 put(0x2e27c0,storage);put(storage,0);put(storage+4,1);put(storage+8,0xffffff9b);
 put(manager+0xe4c,10);put(manager+0xa8,10);put(0x33958c,0x00400020);
 put(0x1f85dc,0x40);put(0x2e1974,1);put(0x24f788,0x120000);
 put(0x12051c,12345);put(0x2151b4,0x130000);put(0x130058,0xdeadbeef);
 const std::string mode=argv[2];
 if(mode=="bad")put(field+0x50,PS2::EE_RAM_SIZE-4);
 if(mode=="stale"){put(field+0x48,0);put(field+0x50,0xffffffff);}
 if(mode=="constructor")put(field+0x50,0);
 const auto before=ee.storage;
 {GameModeTrace trace(argv[1]);ee.m_EE.m_State.nPC=0x124818;
 ee.m_EE.m_State.nGPR[CMIPS::A0].nD0=parent;trace.observe(ee,7);
 if(mode=="normal"||mode=="wrong") {
  ee.m_EE.m_State.nPC=0x16b8f8;
  ee.m_EE.m_State.nGPR[CMIPS::A0].nD0=mode=="wrong"?field:manager;
  trace.observe(ee,7);
 }
 if(mode=="constructor"){ee.m_EE.m_State.nPC=0x169900;trace.observe(ee,7);}
 if(mode=="mode8"||mode=="mode8-wrong"||mode=="mode8-bad-context") {
  if(mode=="mode8-bad-context")put(0x2e27c0,PS2::EE_RAM_SIZE-4);
  ee.m_EE.m_State.nPC=0x12c8f8;
  ee.m_EE.m_State.nGPR[CMIPS::A0].nD0=mode=="mode8-wrong"?field:child;
  trace.observe(ee,8);
 }
 trace.finish();}
 if(before!=ee.storage)throw std::runtime_error("observer mutated guest storage");
 return 0;
 }catch(const std::exception& e){std::cerr<<e.what();return 1;}}
''')
        cls.binary = cls.directory / 'fixture'
        subprocess.run([compiler, '-std=c++20', '-I'+str(cls.directory),
                        '-I'+str(ROOT/'tools/play-oracle'), str(source),
                        '-o', str(cls.binary)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_fixture(self, mode):
        output = self.directory / (mode+'.jsonl')
        run = subprocess.run([str(self.binary), str(output), mode], capture_output=True, text=True)
        return run, output

    def test_entry_timing_pad_half_and_scalars(self):
        run, output = self.run_fixture('normal')
        self.assertEqual(run.returncode, 0, run.stderr)
        header, parent, first = map(json.loads, output.read_text().splitlines())
        self.assertEqual(header['schema'], 'okage-game-mode-observer-v1')
        self.assertEqual(parent['results']['input_scope'], 'snapshot_only')
        row = first['results']
        self.assertEqual(row['input_scope'], 'first_handler_consumes_1c0160')
        self.assertEqual(row['pressed_half_33958e'], '0x00000040')
        self.assertEqual(row['state_e4c'], '0x0000000a')
        self.assertEqual(row['currency_bank4_index3'], '0x00003039')
        self.assertEqual(row['rng_state'], '0xdeadbeef')

    def test_unpublished_constructor_owner_is_null(self):
        run, output = self.run_fixture('constructor')
        self.assertEqual(run.returncode, 0, run.stderr)
        row = json.loads(output.read_text().splitlines()[-1])['results']
        self.assertEqual(row['edge'], 'constructor_entry')
        self.assertIsNone(row['manager_raw_pointer'])

    def test_stale_ui_pointer_outside_phase12_not_dereferenced(self):
        run, output = self.run_fixture('stale')
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIsNone(json.loads(output.read_text().splitlines()[-1])['results']['manager_raw_pointer'])

    def test_owner_extent_and_actual_handler_identity_rejected(self):
        for mode, message in [('bad', 'header/extent'), ('wrong', 'owner mismatch')]:
            with self.subTest(mode=mode):
                run, _ = self.run_fixture(mode)
                self.assertNotEqual(run.returncode, 0)
                self.assertIn(message, run.stderr)

    def test_mode8_snapshot_is_limited_to_linked_live_child_and_storage_context(self):
        run, output = self.run_fixture('mode8')
        self.assertEqual(run.returncode, 0, run.stderr)
        row = json.loads(output.read_text().splitlines()[-1])
        self.assertEqual(row['mode8_child']['ptr'], '0x00113000')
        self.assertEqual(row['mode8_child']['selector_24'], '0x00000003')
        self.assertEqual(row['mode8_child']['counter_30'], '0x00000011')
        self.assertEqual(row['mode8_child']['alpha_3c'], '0x00000040')
        self.assertEqual(row['mode8_child']['rate_40'], '0x3f800000')
        self.assertEqual(row['mode8_child']['image_44'], '0x00223790')
        self.assertEqual(row['storage_device']['continuation_2597ca'], '0x05e4')
        self.assertEqual(row['storage_context_pointer_2e27c0'], '0x00114000')
        self.assertEqual(row['storage_context']['pending'], '0x00000001')
        self.assertEqual(row['storage_context']['result'], '0xffffff9b')

    def test_mode8_rejects_wrong_child_and_invalid_context_pointer(self):
        for mode, message in [('mode8-wrong', 'does not own parent current child'),
                              ('mode8-bad-context', 'context pointer outside EE RAM')]:
            with self.subTest(mode=mode):
                run, _ = self.run_fixture(mode)
                self.assertNotEqual(run.returncode, 0)
                self.assertIn(message, run.stderr)

if __name__ == '__main__':
    unittest.main()
