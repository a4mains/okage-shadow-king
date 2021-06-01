#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>

class CameraScheduleTrace {
    std::ofstream output;
    unsigned count=0;
public:
    static constexpr std::array<uint32,4> breakpoints={0x133a98,0x136aa8,0x134c18,0x1b9c74};
    explicit CameraScheduleTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing camera schedule trace");
        output.open(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot create camera schedule trace");
    }
    void finish() const {if(!count)throw std::runtime_error("Camera schedule observed no callbacks");}
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        auto word=[&](uint32 at){if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Camera schedule RAM bounds");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        const auto& cpu=ee.m_EE.m_State;
        const auto field=word(0x2d9bf4),instance=word(0x337b64),camera=word(0x337d8c);
        if(field<0x100000||field>PS2::EE_RAM_SIZE-0x800||instance>=2048||camera>=5119)
            throw std::runtime_error("Camera schedule requires initialized field/player/camera");
        const auto objects=word(0x21301c),channels=word(0x33760c),player=word(field+0x44);
        if(objects>PS2::EE_RAM_SIZE-2048*0x3c||channels>PS2::EE_RAM_SIZE-5120*0x90||
           player<0x100000||player>PS2::EE_RAM_SIZE-0x284)throw std::runtime_error("Camera schedule object bounds");
        const auto hero=word(objects+instance*0x3c+4)&0xffff;
        if(hero>=5119||++count>4000)throw std::runtime_error("Camera schedule channel/count bounds");
        std::array<uint32,40> row={frame,cpu.nPC,uint32(cpu.nGPR[CMIPS::RA].nD0),word(field+0x48),
            word(0x337d94),word(0x337da4),word(0x33958c)>>16,word(0x33958c)&0xffff};
        const std::array<uint32,7> sources={channels+camera*0x90+0x20,channels+(camera+1)*0x90+0x20,
            channels+hero*0x90+0x20,channels+(hero+1)*0x90+0x20,0x337db0,0x337dd0,0x337de0};
        for(unsigned i=0;i<sources.size();++i)for(unsigned k=0;k<4;++k)row[8+i*4+k]=word(sources[i]+k*4);
        row[36]=word(0x337dac);row[37]=camera;row[38]=instance;row[39]=word(player+0x22c);
        if(!output.write(reinterpret_cast<const char*>(row.data()),sizeof(row)))throw std::runtime_error("Cannot write camera schedule trace");
    }
};
