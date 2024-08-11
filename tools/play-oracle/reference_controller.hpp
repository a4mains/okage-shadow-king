#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>

// Research-only controller entry/return, pad state and window observations.
class ControllerTrace {
    std::ofstream output;
    std::ofstream choices;
    bool pending=false;
    uint32 message=0;
    unsigned count=0;
public:
    static constexpr std::array<uint32,2> breakpoints={0x148dc8,0x11ff38};
    static constexpr std::array<uint32,32> textOffsets={
        0x1900,0xcd0,0xcd4,0xcd8,0x3d08,0xcb0,0xcb4,0x20c,
        0x270,0x274,0x278,0x27c,0x520,0x524,0x528,0x550,
        0x554,0x558,0x560,0x564,0x568,0x868,0x86c,0x870,
        0x878,0x87c,0x880,0x888,0x88c,0x890,0x1970,0x1c70};
    explicit ControllerTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing controller trace");
        output.open(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot create controller trace");
        const auto choicePath=path.string()+".choices.bin";
        if(std::filesystem::exists(choicePath)||std::filesystem::is_symlink(choicePath))throw std::runtime_error("Refusing existing choice trace");
        choices.open(choicePath,std::ios::binary);if(!choices)throw std::runtime_error("Cannot create choice trace");
    }
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        auto word=[&](uint32 at){if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Controller RAM bounds");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        if(cpu.nPC==0x148dc8) {
            if(pending)throw std::runtime_error("Nested controller observation");
            if(uint32(cpu.nGPR[CMIPS::RA].nD0)!=0x11ff38)throw std::runtime_error("Unknown controller caller");
            message=cpu.nGPR[CMIPS::A0].nD0;pending=true;
        } else {if(!pending)return;pending=false;}
        if(++count>40000)throw std::runtime_error("Controller observation limit");
        const auto layer=word(message+0xe48),stage=word(message+0xe8c);
        if(layer>=16 || stage>=16)throw std::runtime_error("Controller layer/stage bounds");
        const auto p=message+0x40+layer*0xe0,handle=word(p);
        if(handle>=8)throw std::runtime_error("Controller text handle bounds");
        const auto object=word(0x200688+handle*4);
        if(!object)throw std::runtime_error("Controller text object missing");
        std::array<uint32,64> r={frame,cpu.nPC,message,layer,handle,object,stage,word(message+0xe4c),
            word(0x33958c)&0xffff,word(0x33958c)>>16,word(0x339590)&0xffff,word(0x339590)>>16,
            word(0x1fcb60),word(0x1fcb64),word(0x1fcb68),word(message+0xed8),
            word(message+0xed4),word(p+0x50),word(p+0x68),word(p+0x6c),word(p+0xb0),word(p+0xd0),
            word(message+0x90),word(message+0x170),word(0x1f85dc),word(0x1f85d0),word(p+0x54),word(p+0x5c),
            word(p+0x10),word(p+0x14),word(p+0x18),word(p+0x60)};
        for(unsigned i=0;i<textOffsets.size();++i)r[32+i]=word(object+textOffsets[i]);
        if(!output.write(reinterpret_cast<const char*>(r.data()),sizeof(r)))throw std::runtime_error("Cannot write controller trace");
        const std::array<uint32,16> choice={frame,cpu.nPC,word(object+0x269c),word(object+0x3e34),
            word(object+0x1e74),word(object+0x1e70),word(0x1fcb6c),word(0x1fcb68),r[8],r[9],r[11],word(object+0x1970),
            word(object+0x3d08),word(message+0xed4),word(object+0xcd4),word(object+0x1900)};
        if(!choices.write(reinterpret_cast<const char*>(choice.data()),sizeof(choice)))throw std::runtime_error("Cannot write choice state");
    }
};
