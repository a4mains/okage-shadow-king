#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

class EffectOrderTrace {
    std::ofstream output;
    unsigned count=0;
public:
    static constexpr std::array<uint32,7> breakpoints={
        0x133a98,0x102218,0x102cf8,0x138130,0x134c18,0x101118,0x1131d0};
    explicit EffectOrderTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))
            throw std::runtime_error("Refusing existing effect-order trace");
        output.open(path,std::ios::binary);
        if(!output)throw std::runtime_error("Cannot create effect-order trace");
    }
    void finish() {
        if(!count)throw std::runtime_error("Effect-order capture observed no callbacks");
        output.flush();
        if(!output)throw std::runtime_error("Cannot flush effect-order trace");
    }
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        auto word=[&](uint32 at) {
            if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Effect-order RAM bounds");
            uint32 value;std::memcpy(&value,ee.m_ram+at,4);return value;
        };
        const auto a0=uint32(cpu.nGPR[CMIPS::A0].nD0),table=word(0x1f849c);
        const auto queued=word(0x1f8494);
        if(queued>84)throw std::runtime_error("Effect-order queue exceeds pool");
        std::array<uint32,100> row{};
        row[0]=frame;row[1]=cpu.nPC;row[2]=uint32(cpu.nGPR[CMIPS::RA].nD0);
        row[3]=a0;row[4]=queued;row[5]=0xffffffff;
        if(cpu.nPC==0x1131d0) {
            if(a0>=84)throw std::runtime_error("Effect-order update handle outside pool");
            row[5]=a0;row[6]=word(table+a0*4);
        } else if(cpu.nPC==0x101118) {
            row[6]=a0;
            for(unsigned i=0;i<84;++i)if(word(table+i*4)==a0){row[5]=i;break;}
            if(row[5]==0xffffffff)throw std::runtime_error("Effect-order follow job outside pool");
        }
        if(row[6]) {
            const auto job=row[6];
            row[7]=word(job);row[8]=word(job+4);row[9]=word(job+0x64);
            row[10]=word(job+0x60);row[11]=word(job+0x68);row[12]=word(job+0x70);
        }
        row[13]=word(0x1f85a4);row[14]=word(word(0x2151b4)+0x58);
        for(unsigned i=0;i<84;++i)row[16+i]=i<queued?word(0x24e630+i*4):0xffffffff;
        if(!output.write(reinterpret_cast<const char*>(row.data()),sizeof(row)))
            throw std::runtime_error("Cannot write effect-order trace");
        ++count;
    }
};
