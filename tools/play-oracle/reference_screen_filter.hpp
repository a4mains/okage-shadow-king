#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>

class ScreenFilterTrace {
    std::ofstream output,packets;
    unsigned count=0;
public:
    static constexpr std::array<uint32,5> breakpoints={0x1b7c08,0x1bb088,0x1942c0,0x193c20,0x193fb0};
    explicit ScreenFilterTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing screen filter trace");
        const auto packetPath=path.string()+".packets.bin";
        if(std::filesystem::exists(packetPath)||std::filesystem::is_symlink(packetPath))throw std::runtime_error("Refusing existing filter packet trace");
        packets.open(packetPath,std::ios::binary);if(!packets)throw std::runtime_error("Cannot create filter packet trace");
        output.open(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot create screen filter trace");
    }
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        auto word=[&](uint32 at){if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Screen filter trace bounds");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        if(++count>12000)throw std::runtime_error("Screen filter observation limit");
        if(cpu.nPC==0x193fb0) {
            const auto base=word(0x332a90);
            std::array<uint32,64> packet={frame,cpu.nPC,base,word(0x332a94)};
            // Completed GIF header and nine A+D register writes, before the mesh.
            for(unsigned k=0;k<40;++k)packet[4+k]=word(base+k*4);
            if(!packets.write(reinterpret_cast<const char*>(packet.data()),sizeof(packet)))throw std::runtime_error("Cannot write filter packet trace");
            return;
        }
        std::array<uint32,48> r={frame,cpu.nPC,uint32(cpu.nGPR[CMIPS::RA].nD0),
            uint32(cpu.nGPR[CMIPS::A0].nD0),uint32(cpu.nGPR[CMIPS::A1].nD0),
            uint32(cpu.nGPR[CMIPS::A2].nD0),uint32(cpu.nGPR[CMIPS::A3].nD0),
            word(0x337e18),word(0x337e1c),word(0x337e14),word(0x332934)};
        if(r[10])for(unsigned k=0;k<4;++k)r[12+k]=word(r[10]+5*0x20+k*4);
        if(cpu.nPC==0x1bb088)for(unsigned k=0;k<4;++k)r[16+k]=word(r[4]+k*4);
        // Texture selector, RGBAQ, texture dimensions, UV steps and GS ALPHA.
        for(unsigned k=0;k<8;++k)r[20+k]=word(0x332ad8+k*4);
        // Mesh base, dimensions, grid subdivision and screen-space transform.
        for(unsigned k=0;k<16;++k)r[28+k]=word(0x332a98+k*4);
        if(!output.write(reinterpret_cast<const char*>(r.data()),sizeof(r)))throw std::runtime_error("Cannot write screen filter trace");
    }
};
