#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <iomanip>
#include <string>
#include <cstdint>

class GameModeTrace {
public:
    static constexpr std::array<uint32,8> breakpoints={
        0x124818,0x101058,0x1354b8,0x133a38,0x149e60,0x149ec8,0x149fd8,0x12e018
    };
    explicit GameModeTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))
            throw std::runtime_error("Refusing existing game-mode trace");
        output.open(path);if(!output)throw std::runtime_error("Cannot create game-mode trace");
        output<<"{\"schema\":\"okage-game-mode-observer-v1\",\"format\":\"JSONL; hex values are strings\"}\n";
    }
    void finish() const {
        if(!count)throw std::runtime_error("Game-mode capture observed no requested callbacks");
        if(count>2000)throw std::runtime_error("Game-mode capture exceeded observation limit");
    }
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        if(++count>2000)throw std::runtime_error("Game-mode observation limit exceeded");
        const auto& cpu=ee.m_EE.m_State;const auto pc=cpu.nPC;
        auto valid=[](uint32 at,unsigned size){return at>=0x100000&&size<=PS2::EE_RAM_SIZE&&at<=PS2::EE_RAM_SIZE-size;};
        auto word=[&](uint32 at)->uint32 {if(!valid(at,4))throw std::runtime_error("Game-mode trace read outside EE RAM");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        auto hex=[](uint32 v){std::ostringstream s;s<<"0x"<<std::hex<<std::setw(8)<<std::setfill('0')<<v;return s.str();};
        auto field=[&](const char* name,uint32 v){output<<",\""<<name<<"\":\""<<hex(v)<<"\"";};
        const uint32 a0=uint32(cpu.nGPR[CMIPS::A0].nD0);
        if(pc==0x124818)parent=a0;
        output<<"{\"frame\":"<<frame<<",\"pc\":\""<<hex(pc)<<"\",\"ra\":\""<<hex(uint32(cpu.nGPR[CMIPS::RA].nD0))<<"\"";
        for(unsigned i=0;i<4;++i){const auto name=std::string("a")+char('0'+i);field(name.c_str(),uint32(cpu.nGPR[CMIPS::A0+i].nD0));}
        field("globalmode_24f738",word(0x24f738));field("pending_24f740",word(0x24f740));
        field("pending_24f744",word(0x24f744));field("pending_24f748",word(0x24f748));
        if(valid(a0,0x20)) {
            field("callback_object",a0);field("object_callback0",word(a0));field("object_callback1",word(a0+4));
            field("object_destructor",word(a0+8));field("object_prev",word(a0+0xc));field("object_next",word(a0+0x10));
            field("object_parent",word(a0+0x14));field("object_first_child",word(a0+0x18));field("object_flags",word(a0+0x1c));
        }
        if(pc==0x1354b8) {
            if(!valid(a0,0x270))throw std::runtime_error("Game-mode player destructor object outside EE RAM");
            const auto h=word(a0+0x26c);output<<",\"player\":\""<<hex(a0)<<"\",\"sound_handle_26c\":"<<int32_t(h);
        }
        if(parent && !valid(parent,0x54))throw std::runtime_error("Game-mode parent pointer outside EE RAM");
        if(parent) {
            field("parent",parent);field("parent_flags",word(parent+0x1c));
            field("parent_phase20",word(parent+0x20));field("parent_child38",word(parent+0x38));
            field("parent_request3c",word(parent+0x3c));field("parent_current40",word(parent+0x40));
            field("parent_arg44",word(parent+0x44));field("parent_queued48",word(parent+0x48));
            field("parent_arg4c",word(parent+0x4c));field("parent_queued50",word(parent+0x50));
            const auto linkedFirst=word(parent+0x18);field("parent_linked_first18",linkedFirst);
            std::set<uint32> seen;unsigned totalNodes=0,childIndex=0;
            auto emitHeader=[&](const std::string& label,uint32 node) {
                if(!valid(node,0x20))throw std::runtime_error("Game-mode linked object pointer outside EE RAM");
                if(!seen.insert(node).second)throw std::runtime_error("Game-mode child tree cycle or duplicate link");
                if(++totalNodes>64)throw std::runtime_error("Game-mode child tree exceeds 64 nodes");
                output<<",\""<<label<<"\":{\"ptr\":\""<<hex(node)<<"\",\"callback0\":\""<<hex(word(node))
                      <<"\",\"callback1\":\""<<hex(word(node+4))<<"\",\"destructor\":\""<<hex(word(node+8))
                      <<"\",\"prev\":\""<<hex(word(node+0xc))<<"\",\"next\":\""<<hex(word(node+0x10))
                      <<"\",\"parent\":\""<<hex(word(node+0x14))<<"\",\"first_child\":\""<<hex(word(node+0x18))
                      <<"\",\"flags\":\""<<hex(word(node+0x1c))<<"\"}";
            };
            uint32 child=linkedFirst;
            while(child) {
                emitHeader("child"+std::to_string(childIndex),child);
                uint32 grandchild=word(child+0x18);unsigned grandchildIndex=0;
                while(grandchild) {
                    emitHeader("grandchild"+std::to_string(childIndex)+"_"+std::to_string(grandchildIndex),grandchild);
                    grandchild=word(grandchild+0x10);++grandchildIndex;
                }
                output<<",\"grandchild_count"<<childIndex<<"\":"<<grandchildIndex;
                child=word(child+0x10);++childIndex;
            }
            output<<",\"child_count\":"<<childIndex<<",\"tree_node_count\":"<<totalNodes;
        }
        output<<"}\n";if(!output)throw std::runtime_error("Cannot write game-mode trace");
    }
private:
    std::ofstream output;unsigned count=0;uint32 parent=0;
};
