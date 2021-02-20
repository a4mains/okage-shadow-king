#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>
class MovementTrace {
    std::ofstream output;unsigned count=0;
public:
    static constexpr std::array<uint32,3> breakpoints={0x1354d8,0x134b70,0x12df80};
    explicit MovementTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing movement trace");
        output.open(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot create movement trace");
    }
    void finish() const {if(!count)throw std::runtime_error("Movement capture observed no callbacks");}
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        auto word=[&](uint32 at){if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Movement trace bounds");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        const auto field=word(0x2d9bf4),player=word(field+0x44);
        if(!player||player>PS2::EE_RAM_SIZE-0x284)throw std::runtime_error("Player state is not initialized");
        if(cpu.nPC!=0x12df80 && uint32(cpu.nGPR[CMIPS::A0].nD0)!=player)
            throw std::runtime_error("Unexpected player callback object");
        if(++count>2000)throw std::runtime_error("Movement observation limit");
        std::array<uint32,64> row={frame,cpu.nPC,uint32(cpu.nGPR[CMIPS::RA].nD0),uint32(cpu.nGPR[CMIPS::A0].nD0),
            word(0x33958c)&0xffff,word(0x3395b8),word(field+0x48),player};
        for(unsigned i=0;i<24;++i)row[8+i]=word(player+0x224+i*4);
        const auto bodies=word(0x337ef8),n=word(0x337ef0);
        if(n!=3)throw std::runtime_error("Movement observer requires three kitchen bodies");
        for(unsigned i=0;i<n;++i) {
            const auto body=bodies+i*0xb0,pos=word(body+0x20);
            for(unsigned k=0;k<4;++k)row[32+i*8+k]=word(pos+k*4);
            row[36+i*8]=word(body+0xc);row[37+i*8]=word(body+0x28);
            row[38+i*8]=word(body+0x40);row[39+i*8]=word(body+0x80);
        }
        const auto instance=word(0x337b64),object=word(0x21301c)+instance*0x3c;
        const auto first=word(object+4)&0xffff,base=word(0x33760c)+first*0x90+0x20;
        for(unsigned k=0;k<4;++k)row[56+k]=word(base+k*4);
        row[60]=word(player+0x220);row[61]=word(player+0x280);
        if(!output.write(reinterpret_cast<const char*>(row.data()),sizeof(row)))throw std::runtime_error("Cannot write movement trace");
    }
};
