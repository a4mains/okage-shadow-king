#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>
class DustTrace {
    std::ofstream output;unsigned count=0;
public:
    static constexpr std::array<uint32,7> breakpoints={0x1012f8,0x1131d0,0x103640,0x133a98,0x134c18,0x102218,0x138130};
    explicit DustTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing dust trace");
        output.open(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot create dust trace");
    }
    void finish() const {if(!count)throw std::runtime_error("Dust capture observed no callbacks");}
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        auto word=[&](uint32 at){if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Dust trace bounds");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        const auto a0=uint32(cpu.nGPR[CMIPS::A0].nD0),a1=uint32(cpu.nGPR[CMIPS::A1].nD0);
        const auto table=word(0x1f849c);
        uint32 job=0,position=0;
        if(cpu.nPC==0x1012f8){if(a0!=17)return;position=a1;}
        else if(cpu.nPC==0x1131d0||cpu.nPC==0x103640) {
            if(a0>=84)return;
            job=word(table+a0*4);if(job==0xffffffff||word(job)!=17)return;
            position=job+0x10;
        }
        const auto player=word(word(0x2d9bf4)+0x44);
        std::array<uint32,16> row={frame,cpu.nPC,a0,a1,word(word(0x2151b4)+0x58),
            word(player+0x230),word(player+0x248),word(0x33958c)};
        if(position)for(unsigned i=0;i<4;++i)row[8+i]=word(position+i*4);
        if(job){row[12]=word(job+4);row[13]=word(job+0x64);row[14]=word(job+0x60);}
        for(unsigned i=0;i<84;++i){const auto p=word(table+i*4);if(p!=0xffffffff&&word(p)==17)++row[15];}
        ++count;
        if(!output.write(reinterpret_cast<const char*>(row.data()),sizeof(row)))throw std::runtime_error("Cannot write dust trace");
    }
};
