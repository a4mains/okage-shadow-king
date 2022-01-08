#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

// Entry-only observation of the shared RNG. No guest memory is modified.
// 0x1f14dc is the jr delay slot containing the store, so it is not treated as
// an executed post-state observation here.
class RngTrace {
    std::ofstream output;
    uint32 rows=0;
public:
    static constexpr uint32 entry=0x1f14b0;
    static constexpr std::array<uint32,1> breakpoints={entry};
    static constexpr uint32 maxRows=1'000'000;

    explicit RngTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))
            throw std::runtime_error("Refusing existing RNG trace");
        output.open(path);
        if(!output)throw std::runtime_error("Cannot create RNG trace");
        output<<"{\"schema\":\"okage-shared-rng-entry\",\"version\":1,"
                "\"entry_pc\":"<<entry<<",\"state_global\":\"0x2151b4\","
                "\"state_offset\":88,\"state_semantics\":\"word before RNG function advances it\","
                "\"post_state\":null,\"post_state_note\":\"No post-store static breakpoint: the store is the jr delay-slot instruction at 0x1f14dc.\","
                "\"scene_id\":null,\"scene_id_note\":\"No stable scene-id field is established by this trace schema.\"}\n";
        if(!output)throw std::runtime_error("Cannot write RNG trace header");
    }
    void finish() {
        output.flush();
        if(!rows)throw std::runtime_error("RNG capture observed no shared RNG calls");
        if(!output)throw std::runtime_error("Cannot finish RNG trace");
    }
    bool observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        if(cpu.nPC!=entry)return false;
        if(rows>=maxRows)throw std::runtime_error("RNG observation limit exceeded");
        auto valid=[](uint32 at,uint32 size){return at<=PS2::EE_RAM_SIZE&&size<=PS2::EE_RAM_SIZE-at;};
        auto word=[&](uint32 at){
            if(!valid(at,4))throw std::runtime_error("RNG trace RAM bounds");
            uint32 value;std::memcpy(&value,ee.m_ram+at,4);return value;
        };
        const auto pointer=word(0x2151b4);
        if(!pointer||!valid(pointer,0x5c))throw std::runtime_error("RNG shared-state pointer bounds");
        const auto state=word(pointer+0x58);
        const auto field=word(0x2d9bf4);
        const bool fieldValid=field&&valid(field,0x4c);
        const auto s1=uint32(cpu.nGPR[CMIPS::S1].nD0);
        const auto ra=uint32(cpu.nGPR[CMIPS::RA].nD0);
        const bool glyphCaller=ra==0x19c754&&valid(s1,0x1904);
        output<<"{\"sequence\":"<<rows<<",\"frame\":"<<frame
              <<",\"pc\":"<<cpu.nPC<<",\"ra\":"<<ra
              <<",\"sp\":"<<uint32(cpu.nGPR[CMIPS::SP].nD0)<<",\"s1\":"<<s1
              <<",\"rng_pointer\":"<<pointer<<",\"state_before\":"<<state
              <<",\"field_pointer\":";
        if(fieldValid)output<<field;else output<<"null";
        output<<",\"field_mode_48\":";
        if(fieldValid)output<<word(field+0x48);else output<<"null";
        output<<",\"field_context_14\":";
        if(fieldValid)output<<word(field+0x14);else output<<"null";
        output<<",\"glyph_text_object\":";
        if(glyphCaller)output<<s1;else output<<"null";
        output<<",\"text_flags_1900\":";
        if(glyphCaller)output<<word(s1+0x1900);else output<<"null";
        output<<",\"glyph_flags_4f8\":";
        if(glyphCaller)output<<word(s1+0x4f8);else output<<"null";
        output<<",\"text_font_20c\":";
        if(glyphCaller)output<<word(s1+0x20c);else output<<"null";
        output<<"}\n";
        if(!output)throw std::runtime_error("Cannot write RNG trace row");
        ++rows;
        return true;
    }
};
