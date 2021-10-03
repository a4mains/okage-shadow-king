#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

class ProjectionTrace {
    std::ofstream output;
    unsigned count=0;
public:
    static constexpr std::array<uint32,9> breakpoints={
        0x1b9be0,0x1b9288,0x183f18,0x183df0,0x102218,0x102cf8,0x1b8dc8,0x185f20,0x101260};
    explicit ProjectionTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))
            throw std::runtime_error("Refusing existing projection trace");
        output.open(path,std::ios::binary);
        if(!output)throw std::runtime_error("Cannot create projection trace");
    }
    void finish() {
        if(!count)throw std::runtime_error("Projection trace observed no calls");
        output.flush();if(!output)throw std::runtime_error("Projection trace flush failed");
    }
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        auto word=[&](uint32 at) {
            if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Projection trace RAM bounds");
            uint32 value;std::memcpy(&value,ee.m_ram+at,4);return value;
        };
        std::array<uint32,300> row{};row[0]=frame;row[1]=cpu.nPC;row[2]=cpu.nGPR[CMIPS::RA].nV[0];
        for(unsigned i=0;i<4;++i)row[3+i]=cpu.nGPR[CMIPS::A0+i].nV[0];
        for(unsigned i=0;i<6;++i)row[7+i]=cpu.nCOP1[12+i];
        for(unsigned i=0;i<16;++i)row[16+i]=word(0x1fe2b4+i*4);
        for(unsigned i=0;i<32;++i) {
            row[32+i]=word(0x337dfc+i*4);row[64+i]=word(0x332220+i*4);row[96+i]=word(0x335350+i*4);
        }
        for(unsigned i=0;i<4;++i)row[128+i]=word(0x1f8484+i*4);
        const auto table=word(0x1f849c);
        for(unsigned i=0;i<84;++i) {
            const auto job=table?word(table+i*4):0xffffffff;
            row[132+i*2]=row[133+i*2]=0xffffffff;
            if(job&&job!=0xffffffff){row[132+i*2]=word(job+4);row[133+i*2]=word(job+0x78);}
        }
        if(!output.write(reinterpret_cast<const char*>(row.data()),sizeof(row)))throw std::runtime_error("Projection trace write failed");
        ++count;
    }
};
