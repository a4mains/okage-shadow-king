#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>
class ActorTrace {
    std::ofstream output;unsigned count=0;int alias;
public:
    static constexpr std::array<uint32,2> renderBreakpoints={0x1856f8,0x185fb0};
    static constexpr std::array<uint32,2> mapBreakpoints={0x133a98,0x1b7a68};
    const std::array<uint32,2>& breakpoints() const {return alias>=0?mapBreakpoints:renderBreakpoints;}
    explicit ActorTrace(const std::filesystem::path& path,int selected=-8):alias(selected) {
        if(alias!=-1 && alias!=-8 && alias!=-9 && (alias<0||alias>=2048))
            throw std::runtime_error("Actor observation requires opening alias or map instance0..2047");
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing actor trace");
        output.open(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot create actor trace");
    }
    void finish() const {if(!count)throw std::runtime_error("Actor capture observed no matching draws");}
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        auto word=[&](uint32 at){if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Actor trace bounds");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        auto half=[&](uint32 at){return (word(at&~3u)>>((at&2)*8))&0xffff;};
        const bool request=alias>=0&&cpu.nPC==0x1b7a68;
        if(alias>=0) {
            if(request) {if(uint32(cpu.nGPR[CMIPS::A2].nD0)!=uint32(alias))return;}
            else {
                const auto field=word(0x2d9bf4);
                if(field<0x100000||field>PS2::EE_RAM_SIZE-0x800)return;
                const auto mode=word(field+0x48);if(mode!=0&&mode!=5)return;
            }
        }
        // Original 0x1bb578 has dedicated globals for player and camera.
        const auto instance=alias>=0 ? uint32(alias) : word(alias==-1 ? 0x337b64 : 0x337b78+(-alias)*4);
        if(instance>=2048)throw std::runtime_error("Actor alias is not initialized");
        const auto object=word(0x21301c)+instance*0x3c;
        if(alias<0&&half(object+2)!=uint32(cpu.nGPR[CMIPS::A0].nD0))return;
        if(++count>2000)throw std::runtime_error("Actor observation limit");
        const auto first=half(object+4),group=half(object+8),context=half(object+10);
        if(context>=192 || group>=1024)throw std::runtime_error("Actor context/group bounds");
        const auto channels=half(word(0x213014)+group*12+6);
        if(channels<4||channels>5120||first+channels>5120||
           (alias<0&&channels!=(alias==-1 ? 228u : (alias==-8 ? 162u : 174u))))
            throw std::runtime_error("Actor trace channel layout mismatch");
        const auto ctx=word(0x337614)+context*24,field=word(0x337614),base=word(0x33760c);
        std::vector<uint32> row={frame,word(field+20)-word(field+8),word(ctx+20)-word(ctx+8),word(ctx),instance,channels,
            request?uint32(cpu.nGPR[CMIPS::A1].nD0):0u,cpu.nPC};
        for(unsigned i=0;i<channels;++i)for(unsigned k=0;k<4;++k)row.push_back(word(base+(first+i)*0x90+0x20+k*4));
        if(!output.write(reinterpret_cast<const char*>(row.data()),row.size()*4))throw std::runtime_error("Cannot write actor trace");
    }
};
