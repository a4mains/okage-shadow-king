#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>
// Research-only chronological observations of field/text/controller scheduling.
class OrderTrace {
    std::ofstream output;
public:
    static constexpr std::array<uint32,5> breakpoints={0x1b5830,0x1980f8,0x198390,0x19d0e8,0x148dc8};
    explicit OrderTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing order trace");
        output.open(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot create order trace");
    }
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        auto word=[&](uint32 at){if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Order RAM bounds");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        const auto text=word(0x20068c),field=word(0x2d9bf4),message=field ? word(field+0x50) : 0;
        if(!text || !message)return;
        if(cpu.nPC==0x1b5830 && word(0x337618)!=word(0x337614))return;
        if(cpu.nPC==0x19d0e8 && uint32(cpu.nGPR[CMIPS::A0].nD0)!=text)return;
        if(cpu.nPC==0x148dc8 && uint32(cpu.nGPR[CMIPS::A0].nD0)!=message)return;
        const std::array<uint32,8> row={frame,cpu.nPC,word(text+0x3d08),word(text+0xcd0),word(text+0xcd4),word(text+0x1900),word(message+0xe4c),word(message+0x1c)};
        if(!output.write(reinterpret_cast<const char*>(row.data()),sizeof(row)))throw std::runtime_error("Cannot write order trace");
    }
};
