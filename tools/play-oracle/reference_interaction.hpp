#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <vector>
class InteractionTrace {
    std::ofstream output;unsigned count=0,npc_capacity=4;
public:
    static constexpr std::array<uint32,5> breakpoints={0x133a98,0x131cac,0x132b00,0x1bb7e0,0x132ca8};
    explicit InteractionTrace(const std::filesystem::path& path,bool wide=false):npc_capacity(wide?7:4) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing interaction trace");
        output.open(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot create interaction trace");
    }
    void finish() const {if(!count)throw std::runtime_error("Interaction capture observed no callbacks");}
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        auto word=[&](uint32 at){if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Interaction trace bounds");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        const auto field=word(0x2d9bf4),player=word(field+0x44),variables=word(0x24f788),n=word(field+0x70);
        if(!player||n>npc_capacity)throw std::runtime_error("Interaction observer NPC count exceeds selected record capacity");
        if(++count>4000)throw std::runtime_error("Interaction observation limit");
        std::vector<uint32> row={frame,cpu.nPC,uint32(cpu.nGPR[CMIPS::RA].nD0),
            uint32(cpu.nGPR[CMIPS::A0].nD0),uint32(cpu.nGPR[CMIPS::A1].nD0),uint32(cpu.nGPR[CMIPS::A2].nD0),
            uint32(cpu.nGPR[CMIPS::S3].nD0),word(0x33958c),word(0x339590),word(field+0x48),
            word(field+0x64),word(field+0x74),word(field+0x50),word(field+0x408),n,
            word(player+0x22c),word(player+0x238),word(player+0x23c),word(player+0x240),word(0x1f85dc)};
        row.resize(64+npc_capacity*16);
        const auto context=word(0x337614);
        row[20]=word(context);row[21]=word(context+20)-word(context+8);
        row[22]=word(variables+0x10);row[23]=word(variables+0x110);
        auto pose=[&](uint32 instance,unsigned offset) {
            const auto object=word(0x21301c)+instance*0x3c,first=word(object+4)&0xffff;
            const auto base=word(0x33760c)+first*0x90+0x20;
            for(unsigned k=0;k<4;++k){row[offset+k]=word(base+k*4);row[offset+4+k]=word(base+0x90+k*4);}
        };
        pose(word(0x337b64),24);
        for(unsigned i=0;i<n;++i) {
            const auto npc=word(field+0x118+i*4),offset=32+i*16;
            for(unsigned k=0;k<8;++k)row[offset+k]=word(npc+0x34+k*4);
            pose(word(npc+0x40),offset+8);
        }
        for(unsigned i=0;i<16;++i)row[32+npc_capacity*16+i]=word(variables+0x510+i*4);
        for(unsigned i=0;i<16;++i)row[48+npc_capacity*16+i]=word(variables+0x10+i*4);
        if(!output.write(reinterpret_cast<const char*>(row.data()),row.size()*4))throw std::runtime_error("Cannot write interaction trace");
    }
};
