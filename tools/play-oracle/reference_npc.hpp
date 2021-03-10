#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>
class NpcTrace {
    std::ofstream output;unsigned count=0;
public:
    static constexpr std::array<uint32,3> breakpoints={0x133a98,0x137090,0x1371fc};
    explicit NpcTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing NPC trace");
        output.open(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot create NPC trace");
    }
    void finish() const {if(!count)throw std::runtime_error("NPC capture observed no callbacks");}
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        auto word=[&](uint32 at){if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("NPC trace bounds");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        auto half=[&](uint32 at){return (word(at&~3u)>>((at&2)*8))&0xffff;};
        const auto field=word(0x2d9bf4),player=word(field+0x44),n=word(field+0x70),vars=word(0x24f788);
        if(!player||n!=7)throw std::runtime_error("NPC observer requires initialized seven-NPC exterior");
        if(++count>4000)throw std::runtime_error("NPC observation limit");
        std::array<uint32,256> row={frame,cpu.nPC,word(field+0x48),n,word(player+0x230),word(vars+0x510+6*4),
            word(word(0x2151b4)+0x58),player};
        const auto base=word(0x33760c);
        auto object=[&](uint32 instance){if(instance>=2048)throw std::runtime_error("NPC instance bounds");return word(0x21301c)+instance*0x3c;};
        auto channels=[&](uint32 first,unsigned out,unsigned number){
            if(first+number>5120)throw std::runtime_error("NPC channel bounds");
            for(unsigned i=0;i<number;++i)for(unsigned k=0;k<4;++k)row[out+i*4+k]=word(base+(first+i)*0x90+0x20+k*4);
        };
        channels(half(object(word(0x337b64))+4),8,2);
        // Field camera channel0 is position, channel1 is rotation.
        channels(word(0x337d8c),16,2);
        row[24]=word(vars+0x510+4*4);row[25]=word(field+0x80);
        row[26]=uint32(cpu.nGPR[CMIPS::A0].nD0);row[27]=uint32(cpu.nGPR[CMIPS::RA].nD0);
        row[28]=word(field+0xec);row[29]=word(field+0xfc);row[30]=word(0x33958c);row[31]=word(vars+0x510);
        for(unsigned i=0;i<n;++i) {
            const auto npc=word(field+0x118+i*4),instance=word(npc+0x40),obj=object(instance),out=32+i*32;
            row[out]=npc;row[out+1]=instance;
            const std::array<uint32,8> offsets={0x34,0x38,0x3c,0x50,0x54,0x58,0x48,0x4c};
            for(unsigned k=0;k<offsets.size();++k)row[out+2+k]=word(npc+offsets[k]);
            row[out+10]=word(obj);row[out+11]=word(obj+0x24);
            channels(half(obj+4),out+12,4);
            const auto context=half(obj+10);if(context>=192)throw std::runtime_error("NPC motion context bounds");
            const auto ctx=word(0x337614)+context*24;
            row[out+28]=word(ctx+20)-word(ctx+8);row[out+29]=word(ctx);
            const auto body=half(obj+0x32);if(body>=word(0x337ef0))throw std::runtime_error("NPC collision body bounds");
            const auto bodyAt=word(0x337ef8)+body*0xb0;
            row[out+30]=word(bodyAt+0xc);row[out+31]=word(bodyAt+4);
        }
        if(!output.write(reinterpret_cast<const char*>(row.data()),sizeof(row)))throw std::runtime_error("Cannot write NPC trace");
    }
};
