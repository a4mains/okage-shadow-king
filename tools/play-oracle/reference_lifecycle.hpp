#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>

class LifecycleTrace {
    std::ofstream output;
    uint32 message=0;
    unsigned count=0;
public:
    static constexpr std::array<uint32,2> breakpoints={0x11fec8,0x11ff5c};
    explicit LifecycleTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing lifecycle trace");
        output.open(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot create lifecycle trace");
    }
    void finish() const {if(!count || message)throw std::runtime_error("Incomplete lifecycle observation");}
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        auto word=[&](uint32 at){if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Lifecycle RAM bounds");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        const auto field=word(0x2d9bf4),current=field ? word(field+0x50) : 0;
        if(cpu.nPC==breakpoints[0]) {
            if(message)throw std::runtime_error("Nested lifecycle scheduler");
            const auto candidate=uint32(cpu.nGPR[CMIPS::A0].nD0);
            if(!current || candidate!=current)return;
            message=candidate;
        } else if(!message)return;
        if(++count>4000)throw std::runtime_error("Lifecycle observation limit");
        const auto layers=word(message+0xe40);
        if(layers!=2)throw std::runtime_error("Lifecycle trace requires two-layer field message");
        std::array<uint32,32> row={frame,cpu.nPC,message,current,word(message+0x1c),word(message+0xe4c),layers,word(message+0xe8c)};
        for(unsigned i=0;i<2;++i) {
            const auto layer=message+0x40+i*0xe0,handle=word(layer);
            if(handle>=8)throw std::runtime_error("Lifecycle text handle bounds");
            const auto object=word(0x200688+handle*4),at=8+i*12;
            if(!object)throw std::runtime_error("Lifecycle missing text object");
            row[at]=handle;row[at+1]=word(layer+0x50);row[at+2]=word(layer+0x60);
            row[at+3]=word(layer+0x68);row[at+4]=object;
            for(unsigned j=0;j<4;++j)row[at+5+j]=word(object+0x270+j*4);
            row[at+9]=word(object+0x1900);row[at+10]=word(object+0x3d08);
        }
        if(!output.write(reinterpret_cast<const char*>(row.data()),sizeof(row)))throw std::runtime_error("Cannot write lifecycle trace");
        if(cpu.nPC==breakpoints[1])message=0;
    }
};
