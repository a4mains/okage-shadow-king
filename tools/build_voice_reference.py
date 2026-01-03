#!/usr/bin/env python3
"""Build isolated, GPL-3.0+ PCSX2 DSP reference kernels under ignored local/."""
import hashlib
from pathlib import Path
import subprocess
import urllib.request
from verify_event import write_exact
from build_spu_reference import REVISION, SOURCE_SHA256

HASHES = {'Mixer.cpp':SOURCE_SHA256,
          'ADSR.cpp':'faea787bf4f132aeb111ac646b0ae4da898e160b602c3e3ae0e9f779d1b86ef9',
          'defs.h':'c99bb3de5ed27243abdaf18384f2d3cf6ce7578fb01fd2b6616da2d99c418960',
          'interpolate_table.h':'3c2f96bdb8aee61a6f988ed508d45811ea8f5bd1f8dac8825c59552bbd2c865a'}

def build():
    root=Path('local/research-audio/pcsx2-reference')
    root.mkdir(parents=True,exist_ok=True)
    files={}
    for name,expected in HASHES.items():
        p=root/name
        if not p.exists():write_exact(p,urllib.request.urlopen('https://raw.githubusercontent.com/PCSX2/pcsx2/'+REVISION+'/pcsx2/SPU2/'+name).read())
        b=p.read_bytes()
        if hashlib.sha256(b).hexdigest()!=expected:raise RuntimeError('Reference source hash: '+name)
        files[name]=b.decode()
    if not (root/'COPYING.GPLv3').exists():
        write_exact(root/'COPYING.GPLv3',urllib.request.urlopen('https://raw.githubusercontent.com/PCSX2/pcsx2/'+REVISION+'/COPYING.GPLv3').read())
    defs=files['defs.h'];adsr=files['ADSR.cpp'];mixer=files['Mixer.cpp']
    prefix=r'''// Research only: PCSX2 DSP kernels, GPL-3.0+.
// Copyright 2002-2026 PCSX2 Dev Team. See COPYING.GPLv3.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using uint=unsigned;
using s8=int8_t;using s16=int16_t;using s32=int32_t;
#define __forceinline inline
#define pxAssume(x) do {if(!(x))throw std::runtime_error("Reference invariant");}while(0)
constexpr bool IsDevBuild=false;
constexpr unsigned XAFLAG_LOOP_START=4,XAFLAG_LOOP_END=1,XAFLAG_LOOP=2;
namespace SPU2 {bool MsgToConsole(){return false;}bool MsgVoiceOff(){return false;}void ConLog(const char*,int){} }
struct StereoOut32 {s32 Left,Right;StereoOut32(s32 l,s32 r):Left(l),Right(r){}};
struct V_VolumeLR {s32 Left,Right;};
struct V_VolumeSlideLR {struct {s32 Value;} Left,Right;};
'''
    types=defs[defs.index('struct V_ADSR\n'):defs.index('// ** Begin Debug-only variables')]
    support=r'''
struct V_Core {V_Voice Voices[24]{};bool IRQEnable=false;u32 IRQA=0,Index=0;struct{u32 ENDX=0;}Regs;};
V_Core Cores[2]{};
constexpr unsigned pcm_WordsPerBlock=8,SPU2_DYN_MEMLINE=0x2800;
struct PcmCacheEntry {bool Validated=false;s32 Prev1=0,Prev2=0;s16 Sampledata[28]{};};
PcmCacheEntry pcm_cache_data[1<<17];
unsigned g_counter_cache_hits=0,g_counter_cache_misses=0,g_counter_cache_ignores=0;
std::vector<s16> memory(1<<20);
s16* GetMemPtr(u32 at){if(at>=memory.size())throw std::runtime_error("Reference RAM bounds");return memory.data()+at;}
void SetIrqCall(int){throw std::runtime_error("Unexpected reference IRQ");}
void V_Voice::Start(){ADSR.Attack();LoopMode=0;SP=0;LoopFlags=0;NextA=StartA|1;Prev1=Prev2=0;SBuffer=nullptr;DecPosRead=DecPosWrite=0;}
void V_Voice::Stop(){ADSR.Value=0;ADSR.Phase=V_ADSR::PHASE_STOPPED;}
'''
    envelope=adsr[adsr.index('static constexpr s32 ADSR_MAX_VOL'):adsr.index('void V_VolumeSlide::RegSet')]
    kernels=mixer[mixer.index('static const s32 tbl_XA_Factor'):mixer.index('// This is Dr. Hell')]
    driver=r'''
std::vector<char> read(const char* p){std::ifstream f(p,std::ios::binary|std::ios::ate);auto n=f.tellg();if(n<0 || n>2*1024*1024)throw std::runtime_error("Reference input bounds");std::vector<char>b(n);f.seekg(0);f.read(b.data(),b.size());return b;}
template<class T> void write(const char* p,const std::vector<T>& b){std::ifstream old(p);if(old.good())throw std::runtime_error("Reference refuses existing output");std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size()*sizeof(T));if(!f)throw std::runtime_error("Reference output");}
int main(int argc,char** argv){try{
 if(argc==3 && std::string(argv[1])=="table"){std::vector<s16>b;for(auto row:interpTable)for(auto x:row)b.push_back(x);write(argv[2],b);return 0;}
 if(argc==7 && std::string(argv[1])=="envelope"){
  V_ADSR a{};a.regADSR1=std::stoul(argv[2],nullptr,0);a.regADSR2=std::stoul(argv[3],nullptr,0);a.UpdateCache();a.Attack();
  unsigned release=std::stoul(argv[4]),count=std::stoul(argv[5]);std::vector<u32> out;
  for(unsigned i=0;i<count;++i){if(i==release)a.Release();if(a.Phase && !a.Calculate(0)){a.Phase=0;a.Value=0;}out.insert(out.end(),{u32(a.Value),a.Phase,a.Counter});}
  write(argv[6],out);return 0;
 }
 if(argc!=10 || std::string(argv[1])!="voice")throw std::runtime_error("Reference arguments");
 auto b=read(argv[2]);if(b.empty() || b.size()%16)throw std::runtime_error("Reference sample block extent");std::memcpy(memory.data(),b.data(),b.size());
 auto& v=Cores[0].Voices[0];v.ADSR.regADSR1=std::stoul(argv[3],nullptr,0);v.ADSR.regADSR2=std::stoul(argv[4],nullptr,0);v.ADSR.UpdateCache();
 v.Pitch=std::stoul(argv[5]);int gain=std::clamp(std::stoi(argv[6])*128,-16384,16383)*2;
 unsigned release=std::stoul(argv[7]),count=std::stoul(argv[8]);v.Start();std::vector<s16> out;
 for(unsigned i=0;i<count;++i){
  if(i==release)v.ADSR.Release();DecodeSamples(0,0);int value=0;
  if(v.ADSR.Phase){value=GetVoiceValues(Cores[0],0);CalculateADSR(Cores[0],0);value=ApplyVolume(value,v.ADSR.Value);value=ApplyVolume(value,gain);}
  UpdatePitch(0,0);ConsumeSamples(Cores[0],0);out.push_back(std::clamp(value,-32768,32767));out.push_back(std::clamp(value,-32768,32767));
 }
 write(argv[9],out);
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''
    code=prefix+'\n#include <cstring>\n'+types+support+'\n#include "interpolate_table.h"\n'+envelope+kernels+driver
    source=root/'voice-reference.cpp'
    if not source.exists() or source.read_text()!=code:source.write_text(code)
    executable=root/'voice-reference'
    subprocess.run(['c++','-std=c++20','-O2',str(source),'-o',str(executable)],check=True)
    return executable

if __name__=='__main__':print(build())
