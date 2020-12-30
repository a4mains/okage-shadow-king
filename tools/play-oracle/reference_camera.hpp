#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>
class CameraTrace {
    std::ofstream output;uint32 position=0,rotation=0;bool pending=false;
public:
    static constexpr std::array<uint32,2> breakpoints={0x1b9760,0x1b9c74};
    explicit CameraTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing camera trace");
        output.open(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot create camera trace");
    }
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        if(cpu.nPC==0x1b9760) {
            if(pending || uint32(cpu.nGPR[CMIPS::RA].nD0)!=0x1b9c74)throw std::runtime_error("Unexpected camera caller");
            position=cpu.nGPR[CMIPS::A0].nD0;rotation=cpu.nGPR[CMIPS::A1].nD0;pending=true;return;
        }
        if(!pending)throw std::runtime_error("Unpaired camera return");pending=false;
        auto word=[&](uint32 at){if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Camera trace bounds");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        const auto channel=word(0x337d8c);if(channel>5120)throw std::runtime_error("Camera channel bounds");
        std::array<uint32,32> row={frame,cpu.nPC,word(0x337d94),word(0x337da4)};
        const std::array<uint32,6> sources={position,rotation,0x337db0,0x337dd0,0x337de0,word(0x33760c)+channel*0x90+0x20};
        for(unsigned j=0;j<sources.size();++j)for(unsigned k=0;k<4;++k)row[4+j*4+k]=word(sources[j]+k*4);
        row[28]=word(0x337dac);row[29]=channel;
        if(!output.write(reinterpret_cast<const char*>(row.data()),sizeof(row)))throw std::runtime_error("Cannot write camera trace");
    }
};
