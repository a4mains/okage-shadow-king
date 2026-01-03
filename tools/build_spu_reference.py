#!/usr/bin/env python3
"""Build a research-only, verbatim PCSX2 block-decoder fixture in ignored local/."""
import hashlib
from pathlib import Path
import subprocess
import urllib.request
from verify_event import write_exact

REVISION = '5b85d6a7585deff715aacd2c0e579a40642cfc25'
SOURCE_SHA256 = '801684f06082b24d2e294b9937c1f5c1b5b53fa6c3b992674b565e8b208d05ee'

def build():
    root = Path('local/research-audio/pcsx2-reference')
    root.mkdir(parents=True, exist_ok=True)
    source = root/'Mixer.cpp'
    base = 'https://raw.githubusercontent.com/PCSX2/pcsx2/'+REVISION+'/'
    if not source.exists():
        write_exact(source, urllib.request.urlopen(base+'pcsx2/SPU2/Mixer.cpp').read())
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise RuntimeError('Pinned PCSX2 source hash mismatch')
    license_path = root/'COPYING.GPLv3'
    if not license_path.exists():
        write_exact(license_path, urllib.request.urlopen(base+'COPYING.GPLv3').read())
    text = data.decode()
    fragment = text[text.index('static const s32 tbl_XA_Factor'):text.index('static void __forceinline IncrementNextA')]
    prefix = '''// Research-only fixture incorporating GPL-3.0+ PCSX2 source.
// Copyright 2002-2026 PCSX2 Dev Team; see COPYING.GPLv3.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>
#include <stdexcept>
using s8=std::int8_t; using s16=std::int16_t; using s32=std::int32_t; using s64=std::int64_t; using u8=std::uint8_t;
#define __forceinline inline
namespace SPU2 { extern float FinalVolume; bool MsgToConsole(){return false;} void ConLog(const char*,int){} }
static bool MsgToConsole(){return false;} static void ConLog(const char*,int){}
template<class T> T GetClamped(T value,T low,T high){return std::clamp(value,low,high);}
template<class T> void Clampify(T& value,T low,T high){value=std::clamp(value,low,high);}
struct StereoOut32 { s32 Left,Right; StereoOut32(s32 left,s32 right):Left(left),Right(right){} };
'''
    driver = '''
int main(int argc,char** argv) {
 try {
  if(argc!=3)throw std::runtime_error("Expected ADPCM input and PCM output");
  std::ifstream in(argv[1],std::ios::binary|std::ios::ate);
  auto size=in.tellg();
  if(size<=0 || size>2*1024*1024 || size%16)throw std::runtime_error("Input extent");
  std::vector<s16> data(static_cast<std::size_t>(size)/2);in.seekg(0);
  if(!in.read(reinterpret_cast<char*>(data.data()),size))throw std::runtime_error("Input read");
  std::vector<s16> pcm;s32 previous=0,older=0;bool ended=false;
  for(std::size_t i=0;i<data.size();i+=8) {
   const unsigned header=static_cast<std::uint16_t>(data[i]);
   if(((header>>4)&15)>4)throw std::runtime_error("Invalid predictor");
   s16 decoded[28];XA_decode_block(decoded,data.data()+i,previous,older);
   pcm.insert(pcm.end(),decoded,decoded+28);
   if(header&0x100) {ended=true;break;}
  }
  if(!ended)throw std::runtime_error("No end block");
  std::ifstream old(argv[2],std::ios::binary|std::ios::ate);
  if(old) {
   if(old.tellg()!=std::streamoff(pcm.size()*2))throw std::runtime_error("Changed existing output extent");
   std::vector<s16> previous(pcm.size());old.seekg(0);
   if(!old.read(reinterpret_cast<char*>(previous.data()),previous.size()*2) || previous!=pcm)
    throw std::runtime_error("Changed existing output");
   return 0;
  }
  std::ofstream out(argv[2],std::ios::binary);
  if(!out.write(reinterpret_cast<const char*>(pcm.data()),pcm.size()*2))throw std::runtime_error("Output write");
 }catch(const std::exception& e){std::cerr<<e.what()<<'\\n';return 1;}
}
'''
    fixture = root/'decoder.cpp'
    write_exact(fixture, (prefix+fragment+driver).encode())
    executable = root/'decoder'
    subprocess.run(['c++', '-std=c++20', '-O2', str(fixture), '-o', str(executable)], check=True)
    return executable

if __name__ == '__main__':
    print(build())
