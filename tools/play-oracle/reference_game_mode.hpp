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
#include <cstring>
#include <stdexcept>

class GameModeTrace {
public:
    static constexpr std::array<uint32,14> breakpoints={
        0x124818,0x101058,0x1354b8,0x133a38,0x149e60,0x149ec8,0x149fd8,0x12e018,0x169900,0x16b8f8,0x16b6c8,0x12c8f8,
        0x11e480,0x11c2c8
    };
    explicit GameModeTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))
            throw std::runtime_error("Refusing existing game-mode trace");
        output.open(path);if(!output)throw std::runtime_error("Cannot create game-mode trace");
        output<<"{\"schema\":\"okage-game-mode-observer-v1\",\"format\":\"JSONL; hex values are strings\","
              <<"\"results_extension\":\"source-results-entry-v1\",\"row_limit\":8000,"
              <<"\"mode8_extension\":\"12c8f8 callback snapshots child+24/+30/+3c/+40/+44, device words2597a8/ac/b0, continuation2597ca and pointed thread context control/pending/result words. Only emitted for the linked current child.\","
              <<"\"results_timing\":\"124818 is global-parent entry before descendants;169900 constructor entry;16b8f8 first handler entry before1c0160(0);16b6c8 second handler entry before16a0c0 copies command. No completed-callback claim.\","
              <<"\"pad_source\":\"1c0160(0) reads unsignedhalf33958e;16b8f8 masks with word1f85dc. Parent samples are not consumed-input observations.\","
              <<"\"restore_extension\":\"11e480 Restore input-handler and 11c2c8 second-callback entry snapshots; manager identity is resolved from live mode-10 parent+34 and checked against descriptor2234f0. Captures globals, storage result/context, and controller source words without guest writes.\","
              <<"\"elf_sha256\":\"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5\"}\n";
    }
    void finish() const {
        if(!count)throw std::runtime_error("Game-mode capture observed no requested callbacks");
        if(count>8000)throw std::runtime_error("Game-mode capture exceeded observation limit");
    }
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        if(++count>8000)throw std::runtime_error("Game-mode observation limit exceeded");
        const auto& cpu=ee.m_EE.m_State;const auto pc=cpu.nPC;
        auto valid=[](uint32 at,unsigned size){return at>=0x100000&&size<=PS2::EE_RAM_SIZE&&at<=PS2::EE_RAM_SIZE-size;};
        auto word=[&](uint32 at)->uint32 {if(!valid(at,4))throw std::runtime_error("Game-mode trace read outside EE RAM");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        auto half=[&](uint32 at)->uint16_t {if(!valid(at,2))throw std::runtime_error("Game-mode trace halfword read outside EE RAM");uint16_t v;std::memcpy(&v,ee.m_ram+at,2);return v;};
        auto hex=[](uint32 v){std::ostringstream s;s<<"0x"<<std::hex<<std::setw(8)<<std::setfill('0')<<v;return s.str();};
        auto hex16=[](uint16_t v){std::ostringstream s;s<<"0x"<<std::hex<<std::setw(4)<<std::setfill('0')<<v;return s.str();};
        auto field=[&](const char* name,uint32 v){output<<",\""<<name<<"\":\""<<hex(v)<<"\"";};
        const uint32 a0=uint32(cpu.nGPR[CMIPS::A0].nD0);
        if(pc==0x124818) {
            if(parent!=a0) { restore_manager=0;restore_input_count=0;restore_second_count=0; }
            parent=a0;
        }
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
        if(pc==0x12c8f8) {
            if(!parent||!valid(parent,0x54))throw std::runtime_error("Mode8 callback has no captured game-mode parent");
            const auto current=word(parent+0x38);
            if(!current||!valid(current,0x48))throw std::runtime_error("Mode8 callback current child is null/outside EE RAM");
            if(a0!=current)throw std::runtime_error("Mode8 callback does not own parent current child");
            if(word(current+4)!=0x12c8f8||word(current+8)!=0x12c8d8)
                throw std::runtime_error("Mode8 callback child descriptor mismatch");
            output<<",\"mode8_child\":{\"ptr\":\""<<hex(current)<<"\"";
            field("selector_24",word(current+0x24));field("counter_30",word(current+0x30));
            field("alpha_3c",word(current+0x3c));field("rate_40",word(current+0x40));
            field("image_44",word(current+0x44));output<<"}";
            output<<",\"storage_device\":{\"type_2597a8\":\""<<hex(word(0x2597a8))
                  <<"\",\"value_2597ac\":\""<<hex(word(0x2597ac))
                  <<"\",\"capacity_2597b0\":\""<<hex(word(0x2597b0))
                  <<"\",\"continuation_2597ca\":\""<<hex16(half(0x2597ca))<<"\"}";
            const auto context=word(0x2e27c0);field("storage_context_pointer_2e27c0",context);
            if(context==0)output<<",\"storage_context\":null";
            else {
                if(!valid(context,0xc))throw std::runtime_error("Storage worker context pointer outside EE RAM");
                output<<",\"storage_context\":{\"control\":\""<<hex(word(context))
                      <<"\",\"pending\":\""<<hex(word(context+4))
                      <<"\",\"result\":\""<<hex(word(context+8))<<"\"}";
            }
        }
        if(pc==0x11e480||pc==0x11c2c8) {
            if(!parent||!valid(parent,0x54))throw std::runtime_error("Restore callback has no captured game-mode parent");
            if(word(parent+0x24)!=10)throw std::runtime_error("Restore callback is not owned by a live mode-10 parent");
            const auto manager=word(parent+0x34);
            if(!manager||!valid(manager,0xef0))throw std::runtime_error("Restore parent manager is null/outside EE RAM");
            const std::array<uint32,4> expected_descriptor={0x11f668,0x11fec8,0x120ad0,0x120b48};
            for(unsigned i=0;i<expected_descriptor.size();++i)
                if(word(0x2234f0+i*4)!=expected_descriptor[i])throw std::runtime_error("Restore manager descriptor2234f0 mismatch");
            if(word(manager)!=expected_descriptor[1]||word(manager+4)!=expected_descriptor[2]||
               word(manager+8)!=expected_descriptor[3])
                throw std::runtime_error("Restore manager callbacks differ from descriptor2234f0");
            const char* edge=pc==0x11e480?"input_handler_entry":"second_callback_entry";
            if(pc==0x11e480) {
                if(a0!=manager)throw std::runtime_error("Restore input handler A0 is not the mode-10 manager");
                if(restore_manager&&restore_manager!=manager)throw std::runtime_error("Restore manager changed without a new observer instance");
                restore_manager=manager;
                ++restore_input_count;
            } else {
                if(!restore_manager||restore_manager!=manager)throw std::runtime_error("Restore second callback has no matching live manager observed at11e480");
                ++restore_second_count;
            }
            const auto layer_count=word(manager+0xe40);
            const auto layer_index=word(manager+0xe48);
            if(!layer_count||layer_count>16||static_cast<std::int32_t>(layer_index)<0||layer_index>=layer_count)
                throw std::runtime_error("Restore manager layer count/index is outside its owned object");
            const auto layer=manager+0x40+layer_index*0xe0;
            if(!valid(layer,0xe0)||layer+0xe0>manager+0xef0)throw std::runtime_error("Restore manager layer record outside manager extent");
            output<<",\"restore\":{\"edge\":\""<<edge<<"\",\"input_handler_ordinal\":"<<restore_input_count
                  <<",\"second_callback_ordinal\":"<<restore_second_count;
            field("parent_mode24",word(parent+0x24));field("parent_manager34",manager);
            field("manager_flags1c",word(manager+0x1c));field("manager_layer_count_e40",layer_count);
            field("manager_layer_index_e48",layer_index);field("manager_dispatch_state_e4c",word(manager+0xe4c));
            field("manager_layer_state_prefix_e90",word(manager+0xe90));
            field("manager_layer_state_cursor_e8c",word(manager+0xe8c));
            field("manager_parent14",word(manager+0x14));
            output<<",\"descriptor2234f0\":[";
            for(unsigned i=0;i<expected_descriptor.size();++i){if(i)output<<',';output<<'"'<<hex(word(0x2234f0+i*4))<<'"';}
            output<<"],\"manager_layer_words\":[";
            for(unsigned i=0;i<0xe0/4;++i){if(i)output<<',';output<<'"'<<hex(word(layer+i*4))<<'"';}
            output<<"]";
            field("global_1f89a0",word(0x1f89a0));field("global_1f89a4",word(0x1f89a4));
            field("global_1f89a8",word(0x1f89a8));field("global_1f89ac",word(0x1f89ac));
            field("global_24ecc0",word(0x24ecc0));field("global_24ecc4",word(0x24ecc4));
            field("global_24ecd8",word(0x24ecd8));field("global_24ee08",word(0x24ee08));
            field("global_24ee10",word(0x24ee10));field("global_24ee14",word(0x24ee14));
            field("global_24ee18",word(0x24ee18));field("global_24ee40",word(0x24ee40));
            field("global_24ee44",word(0x24ee44));field("global_24ee48",word(0x24ee48));
            field("global_24ee50",word(0x24ee50));field("global_24ee54",word(0x24ee54));
            field("global_24ee58",word(0x24ee58));field("global_24ee60",word(0x24ee60));
            field("global_24ee64",word(0x24ee64));field("global_24ee68",word(0x24ee68));
            field("storage_type_24ee78",word(0x24ee78));field("storage_result_24ee7c",word(0x24ee7c));
            field("storage_continuation_24ee9a",half(0x24ee9a));
            output<<",\"controller\":{\"word_339588\":\""<<hex(word(0x339588))
                  <<"\",\"held_33958c\":\""<<hex(word(0x33958c))
                  <<"\",\"pressed_33958e\":\""<<hex16(half(0x33958e))
                  <<"\",\"word_339590\":\""<<hex(word(0x339590))<<"\"}";
            const auto context=word(0x2e27c0);field("storage_context_pointer_2e27c0",context);
            if(!context)output<<",\"storage_context\":null";
            else {
                if(!valid(context,0xc))throw std::runtime_error("Restore storage worker context pointer outside EE RAM");
                output<<",\"storage_context\":{\"control\":\""<<hex(word(context))
                      <<"\",\"pending\":\""<<hex(word(context+4))
                      <<"\",\"result\":\""<<hex(word(context+8))<<"\"}";
            }
            output<<"}";
        }
        if(parent && !valid(parent,0x54))throw std::runtime_error("Game-mode parent pointer outside EE RAM");
        uint32 resultsField=0;
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
                if(word(node+4)==0x133a98&&word(node+8)==0x133a38) {
                    if(resultsField&&resultsField!=node)throw std::runtime_error("Multiple linked field owners in game-mode trace");
                    resultsField=node;
                }
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
        if(pc==0x124818||pc==0x169900||pc==0x16b8f8||pc==0x16b6c8) {
            const char* edge=pc==0x124818?"parent_entry":pc==0x169900?"constructor_entry":
                pc==0x16b8f8?"first_handler_entry":"second_handler_entry";
            output<<",\"results\":{\"edge\":\""<<edge<<"\"";
            field("timer_1fdda8",word(0x1fdda8));field("advance_1fddac",word(0x1fddac));
            field("loader_ready_2e1974",word(0x2e1974));field("notice_1fddd4",word(0x1fddd4));
            field("notice_row_1fddc0",word(0x1fddc0));field("notice_wait_1fddc4",word(0x1fddc4));
            field("reward_display_1fddb4",word(0x1fddb4));field("money_display_1fddb8",word(0x1fddb8));
            field("copied_command_2e1970",word(0x2e1970));
            const auto vars=word(0x24f788),rng=word(0x2151b4);
            if(vars)field("currency_bank4_index3",word(vars+0x51c));
            else output<<",\"currency_bank4_index3\":null";
            if(rng)field("rng_state",word(rng+0x58));else output<<",\"rng_state\":null";
            const auto pad=word(0x33958c),mask=word(0x1f85dc);
            field("pad0_339588",word(0x339588));field("pad0_33958c",pad);
            field("pad0_339590",word(0x339590));field("confirm_mask_1f85dc",mask);
            field("pressed_half_33958e",pad>>16);
            output<<",\"input_scope\":\""<<(pc==0x16b8f8?"first_handler_consumes_1c0160":"snapshot_only")<<"\"";
            uint32 manager=0;
            if(resultsField) {
                if(!valid(resultsField,0x54))throw std::runtime_error("Results field extent outside EE RAM");
                field("field_raw_pointer",resultsField);
                field("field_phase48",word(resultsField+0x48));field("field_complete30",word(resultsField+0x30));
                field("field_ui_stage34",word(resultsField+0x34));field("field_counter38",word(resultsField+0x38));
                if(word(resultsField+0x48)==0x12)manager=word(resultsField+0x50);
            } else output<<",\"field_raw_pointer\":null";
            if(manager) {
                if(!valid(manager,0xef0)||word(manager)!=0x11fec8||word(manager+4)!=0x120ad0||word(manager+8)!=0x120b48)
                    throw std::runtime_error("Results manager header/extent mismatch");
                if(word(manager+0xe40)==1&&word(manager+0xa0)==11) {
                    if(pc==0x16b8f8&&a0!=manager)throw std::runtime_error("Results first handler owner mismatch");
                    field("manager_raw_pointer",manager);field("manager_flags1c",word(manager+0x1c));
                    field("state_e4c",word(manager+0xe4c));field("command_a8",word(manager+0xa8));
                    field("previous_a4",word(manager+0xa4));field("dispatch_cursor_e8c",word(manager+0xe8c));
                    field("selected_layer_e48",word(manager+0xe48));
                } else output<<",\"manager_raw_pointer\":null";
            } else output<<",\"manager_raw_pointer\":null";
            output<<"}";
        }
        output<<"}\n";if(!output)throw std::runtime_error("Cannot write game-mode trace");
    }
private:
    std::ofstream output;unsigned count=0;uint32 parent=0,restore_manager=0;
    unsigned restore_input_count=0,restore_second_count=0;
};
