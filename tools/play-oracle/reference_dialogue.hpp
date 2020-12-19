#pragma once
#include "ee/Ee_SubSystem.h"
#include <filesystem>
#include <fstream>
#include <array>

class DialogueTrace {
    std::ofstream output;
    std::array<uint32,12> input{};
    bool pending=false;
    unsigned count=0;
public:
    explicit DialogueTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path) || std::filesystem::is_symlink(path))
            throw std::runtime_error("Refusing existing dialogue trace output");
        output.open(path,std::ios::binary);
        if(!output)throw std::runtime_error("Cannot create dialogue trace");
    }
    void observe(Ee::CSubSystem& ee) {
        const auto& cpu=ee.m_EE.m_State;
        auto word=[&](uint32 at){
            if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Dialogue trace RAM bounds");
            uint32 value;std::memcpy(&value,ee.m_ram+at,4);return value;
        };
        const auto field=word(0x2d9bf4);
        if(cpu.nPC==0x130458) {
            if(pending || count>=10000)throw std::runtime_error("Dialogue trace nested call or limit");
            if(uint32(cpu.nGPR[CMIPS::RA].nD0)!=0x1b59f0)
                throw std::runtime_error("Unexpected dialogue-wait caller");
            input={uint32(cpu.nGPR[CMIPS::A0].nD0),word(field+0x48),word(field+0x30),
                   word(field+0x54),word(field+0x58),word(field+0x5c),0,0,0,0,0xffffffff,0};
            const auto message=word(field+0x50);
            if(message) {
                input[6]=1;input[7]=word(message+0x1c);input[8]=word(message+0xe40);
                if(input[8]>16)throw std::runtime_error("Unobserved dialogue layer count");
                for(unsigned i=0;i<input[8];++i)
                    if(word(message+0x90+i*0xe0)&0x40)input[9]|=1u<<i;
                const int32 handle=word(message+0x120);
                if(handle>=0) {
                    if(handle>4095)throw std::runtime_error("Dialogue text handle bounds");
                    const auto text=word(0x200688+uint32(handle)*4);
                    if(text) {input[10]=0;input[11]=word(text+0x3d08);}
                }
            }
            pending=true;return;
        }
        if(cpu.nPC!=0x1b59f0 || !pending)throw std::runtime_error("Unexpected dialogue trace return");
        const std::array<uint32,4> result={word(field+0x54),word(field+0x58),word(field+0x5c),uint32(cpu.nGPR[CMIPS::V0].nD0)};
        if(!output.write(reinterpret_cast<const char*>(input.data()),sizeof(input)) ||
           !output.write(reinterpret_cast<const char*>(result.data()),sizeof(result)))
            throw std::runtime_error("Cannot write dialogue trace record");
        pending=false;++count;
    }
};
