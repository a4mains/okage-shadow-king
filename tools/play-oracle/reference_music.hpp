#pragma once
#include "iop/Iop_SubSystem.h"
#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

// Research-only original RXSNDDRV sequencer observations. No guest state changes.
class MusicTrace {
    std::ofstream output;
    std::vector<uint8> sequence;
    unsigned ticks=0,events=0;
    uint32 activeTrack=0;
    static std::vector<uint8> pinned(const char* path,const char* expected) {
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(!file || file.tellg()<=0 || file.tellg()>1024*1024)throw std::runtime_error("Music research input missing/too large");
        std::vector<uint8> bytes(static_cast<std::size_t>(file.tellg()));file.seekg(0);
        if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("Short music research input");
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes.data(),bytes.size(),digest);
        std::ostringstream hash;for(auto v:digest)hash<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(v);
        if(hash.str()!=expected)throw std::runtime_error("Music research input hash mismatch");
        return bytes;
    }
public:
    uint32 base=0;
    static constexpr std::array<uint32,4> offsets={0x4c9c,0x4cec,0x4cf8,0x4a30};
    explicit MusicTrace(const std::filesystem::path& path,Iop::CSubSystem& iop,unsigned kind=0) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing music trace");
        const auto irx=pinned("local/extracted/MODULES/RXSNDDRV.IRX","7fe8b5a33c2f8700d1f964cf5ce2f66942f8f97d952e86e6dc0ad5d4f74becb2");
        if(kind>2)throw std::runtime_error("Unknown music trace corpus");
        sequence=kind==2 ? pinned("local/audio-input/SNDDATA/BGM/JGL_GET.XSQ","b15448498b8a4738cb98d72fda4a595daf6fdc8bee644d732633f631f9e2f407") :
            kind==1 ? pinned("local/audio-input/SNDDATA/BGM/FLD_NMA.XSQ","17704d69deb1e516e216a2459ac83c8e80773582916de434ad98b860fb8ab16f") :
            pinned("local/audio-input/SNDDATA/BGM/EVS_MSH.XSQ","908a259c61b333bad7c570b7c1c33f9e954d5c93811be52228643b8323131cf2");
        unsigned matches=0;
        for(uint32 at=0;at+233424<PS2::IOP_RAM_SIZE;at+=16) {
            bool same=true;
            for(auto offset:{0x4ea0u,0x4c9cu,0x7f70u})
                same&=std::memcmp(iop.m_ram+at+offset,irx.data()+176+offset,16)==0;
            if(same){base=at;++matches;}
        }
        if(matches!=1)throw std::runtime_error("Cannot uniquely locate loaded RXSNDDRV");
        output.open(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot create music trace");
    }
    void observe(Iop::CSubSystem& iop,unsigned frame) {
        const auto& cpu=iop.m_cpu.m_State;const auto pc=cpu.nPC-base;
        auto word=[&](uint32 at){if(at>PS2::IOP_RAM_SIZE-4)throw std::runtime_error("Music RAM bounds");uint32 v;std::memcpy(&v,iop.m_ram+at,4);return v;};
        if(pc==0x4a30 && !activeTrack)return;
        const auto slot=uint32(cpu.nGPR[pc==0x4c9c ? CMIPS::A0 : (pc==0x4a30 ? CMIPS::S5 : CMIPS::S2)].nD0);
        const auto track=uint32(cpu.nGPR[pc==0x4c9c ? CMIPS::A1 : (pc==0x4a30 ? CMIPS::S0 : CMIPS::S1)].nD0);
        const auto descriptor=word(track+4);
        if(descriptor<36 || descriptor-36+sequence.size()>PS2::IOP_RAM_SIZE)return;
        const auto source=descriptor-36;
        if(std::memcmp(iop.m_ram+source,sequence.data(),4) ||
           std::memcmp(iop.m_ram+source+52,sequence.data()+52,sequence.size()-52))return;
        if(pc==0x4c9c) {if(activeTrack)throw std::runtime_error("Nested music tick");activeTrack=track;++ticks;}
        else if(activeTrack!=track)throw std::runtime_error("Music observation outside tick");
        if(ticks>12000 || events>20000)throw std::runtime_error("Music trace budget exceeded");
        const bool event=pc==0x4cec || pc==0x4cf8;
        if(event)++events;
        const auto cursor=word(track+8);if(cursor>=PS2::IOP_RAM_SIZE)throw std::runtime_error("Music cursor bounds");
        const auto next=iop.m_ram[cursor];
        std::vector<uint32> row={frame,pc,ticks,word(slot+4),slot,track,word(track+8)-source,source,
            event ? uint32(next&0x80 ? next : iop.m_ram[track+15]) : 0u,0};
        for(unsigned at=8;at<=0x24;at+=4)row.push_back(word(slot+at));
        for(unsigned at=12;at<=0x20;at+=4)row.push_back(word(track+at));
        for(unsigned at=0x2c;at<0x2ac;at+=4)row.push_back(word(slot+at));
        if(row.size()!=184)throw std::runtime_error("Music row layout mismatch");
        if(!output.write(reinterpret_cast<const char*>(row.data()),row.size()*4))throw std::runtime_error("Cannot write music trace");
        if(pc==0x4a30)activeTrack=0;
    }
    void finish() const {if(activeTrack || !ticks || !events)throw std::runtime_error("Incomplete/empty music trace");}
};
