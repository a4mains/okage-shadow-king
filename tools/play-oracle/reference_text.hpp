#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>

class TextTrace {
    std::ofstream output;
    bool drawing=false;
    unsigned count=0;
public:
    static constexpr std::array<uint32,6> breakpoints={0x1980f8,0x198390,0x19d0e8,0x1984f4,0x1985c0,0x1a51a0};
    static constexpr std::array<uint32,30> offsets={
        0x1900,0x25c,0x3d08,0x269c,0x3e34,0x3bf4,0xcd0,0xcd4,0xcd8,0xce8,
        0x260,0x264,0x268,0x26c,0x270,0x27c,0xcc0,0xcc4,0x3e38,0xcb0,
        0xcb4,0x22c,0x4f8,0x3bec,0x26a0,0x20c,0x254,0x258,0x1904,0x3e24};
    explicit TextTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path) || std::filesystem::is_symlink(path))
            throw std::runtime_error("Refusing existing text trace");
        output.open(path,std::ios::binary);
        if(!output)throw std::runtime_error("Cannot create text trace");
    }
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        auto word=[&](uint32 at){
            if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Text trace RAM bounds");
            uint32 result;std::memcpy(&result,ee.m_ram+at,4);return result;
        };
        const auto& cpu=ee.m_EE.m_State;
        const uint32 object=word(0x20068c),a0=cpu.nGPR[CMIPS::A0].nD0;
        if(!object)throw std::runtime_error("Text trace requires active opening handle 1");
        if(cpu.nPC==0x19d0e8) {
            if(a0!=object)return;
            if(drawing)throw std::runtime_error("Nested text draw observation");
            drawing=true;
        }
        if(cpu.nPC==0x1984f4) {
            if(!drawing)return;
            drawing=false;
        }
        if((cpu.nPC==0x1985c0 || cpu.nPC==0x1a51a0) && a0!=1)return;
        if(++count>10000)throw std::runtime_error("Text trace observation limit");
        const auto field=word(0x2d9bf4),message=word(field+0x50);
        std::array<uint32,40> row={frame,cpu.nPC,uint32(cpu.nGPR[CMIPS::RA].nD0),a0,
            uint32(cpu.nGPR[CMIPS::A1].nD0),object,message,message ? word(message+0x90) : 0,
            message ? word(message+0x170) : 0,word(0x2005c0)};
        for(unsigned i=0;i<offsets.size();++i)row[10+i]=word(object+offsets[i]);
        if(!output.write(reinterpret_cast<const char*>(row.data()),sizeof(row)))
            throw std::runtime_error("Cannot write text observation");
    }
};
