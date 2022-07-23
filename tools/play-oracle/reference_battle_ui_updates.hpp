#pragma once
#include "reference_battle_ui.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

class BattleUiUpdatesTrace {
public:
    static constexpr uint32 firstCallback=0x11fec8;
    static constexpr uint32 secondCallback=0x120ad0;
    static constexpr std::array<uint32,2> entryBreakpoints={firstCallback,secondCallback};
    static constexpr unsigned completedPerCallback=2;

    explicit BattleUiUpdatesTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))
            fail("refusing existing UI-update trace");
        output.open(path);
        if(!output)fail("cannot create UI-update trace");
        output<<"{\"schema\":\"okage-battle-ui-updates-v1\","
              <<"\"scope\":\"first two completed calls to each battle UI-manager callback for the root manager\","
              <<"\"pairing\":\"entry RA is the exact dynamic caller continuation; return requires matching PC, restored SP, same battle root, and same root+8060 manager\","
              <<"\"callbacks\":[\"0x11fec8 first-layer dispatcher\",\"0x120ad0 second-layer dispatcher\"],"
              <<"\"stream_extent_note\":\"External authored streams are copied only for their source-established five-word prefix. For each window whose active source is its embedded stream, copy cursor+1 words (including the terminator), with cursor<=511 and at most 512 words; this is not an allocation-boundary claim\","
              <<"\"embedded_stream_note\":\"Opcode 0x17 pointer operands are normalized to offsets within the validated RUW raw widget-record owner; all non-0x17 words remain exact source words. Glyph output allocations are copied at source extent (window+3c00 plus one) times 256 bytes; capacity is capped at 32\","
              <<"\"pad_scope\":\"Only sourced globals 0x33958c, 0x339590, 0x1fcb60, 0x1fcb64, and 0x1fcb68 are captured\","
              <<"\"elf_sha256\":\"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5\"}\n";
        flush();
    }

    bool recognizes(uint32 pc) const {
        if(pc==firstCallback||pc==secondCallback)return true;
        return active&&pc==active->returnPc;
    }

    void observe(Ee::CSubSystem& ee,unsigned frame,std::vector<uint32>& loaders) {
        const auto& cpu=ee.m_EE.m_State;
        const uint32 pc=uint32(cpu.nPC),sp=uint32(cpu.nGPR[CMIPS::SP].nD0);
        auto reg=[&](unsigned index){return uint32(cpu.nGPR[index].nD0);};
        BattleUiTrace::Memory memory{ee};
        if(active) {
            if(pc==active->returnPc) {
                if(sp!=active->sp||reg(CMIPS::RA)!=active->returnPc||
                   memory.word(0x1fce60)!=active->root||
                   memory.word(active->root+0x8060)!=active->manager)
                    fail("unpaired UI callback caller continuation");
                snapshot(active->kind==0?"first_callback_complete":"second_callback_complete",
                         frame,pc,reg(CMIPS::RA),sp,cpu.nFCSR,memory,*active,true);
                ++completed[active->kind];
                const auto returnPc=active->returnPc;
                const bool owned=active->ownsBreakpoint;
                active.reset();
                if(owned) {
                    ee.m_EE.ToggleBreakpoint(returnPc);
                    loaders.erase(std::remove(loaders.begin(),loaders.end(),returnPc),loaders.end());
                }
                return;
            }
            if(pc==firstCallback||pc==secondCallback)
                fail("nested battle UI-manager callback");
            return;
        }
        if(pc!=firstCallback&&pc!=secondCallback)return;
        const unsigned kind=pc==firstCallback?0:1;
        if(completed[kind]>=completedPerCallback)return;
        const uint32 root=memory.word(0x1fce60);
        if(!root)return;
        memory.require(root,0x81d0);
        const uint32 manager=memory.word(root+0x8060);
        const uint32 candidate=reg(CMIPS::A0);
        if(!manager||candidate!=manager)return;
        memory.require(manager,0xef0);
        if(memory.word(manager+0xe40)!=2)
            fail("target UI manager no longer has the two authored layers");
        validateUpdateManager(memory,manager);
        const uint32 returnPc=reg(CMIPS::RA);
        if((returnPc&3)||returnPc<0x100000||returnPc>=PS2::EE_RAM_SIZE||returnPc==pc)
            fail("target UI callback has an invalid live return PC");
        const bool alreadyInstalled=std::find(loaders.begin(),loaders.end(),returnPc)!=loaders.end();
        if(!alreadyInstalled) {
            ee.m_EE.ToggleBreakpoint(returnPc);
            loaders.push_back(returnPc);
        }
        Call call{kind,root,manager,sp,returnPc,!alreadyInstalled,{}};
        std::memcpy(call.partySelection.data(),ee.m_ram+0x1fae18,call.partySelection.size());
        active=call;
        ++entered[kind];
        snapshot(kind==0?"first_callback_entry":"second_callback_entry",
                 frame,pc,returnPc,sp,cpu.nFCSR,memory,*active,false);
    }

    void finish() {
        if(active||entered[0]!=completedPerCallback||entered[1]!=completedPerCallback||
           completed[0]!=completedPerCallback||completed[1]!=completedPerCallback)
            fail("requires two complete entry/return pairs for each UI callback");
        output<<"{\"event\":\"finish\",\"first_entered\":"<<entered[0]
              <<",\"first_completed\":"<<completed[0]<<",\"second_entered\":"<<entered[1]
              <<",\"second_completed\":"<<completed[1]<<",\"snapshots\":"<<rows<<"}\n";
        flush();
    }

private:
    struct Call {
        unsigned kind;
        uint32 root,manager,sp,returnPc;
        bool ownsBreakpoint;
        std::array<uint8,0x120> partySelection{};
    };
    std::ofstream output;
    std::optional<Call> active;
    std::array<unsigned,2> entered{},completed{};
    unsigned rows=0;
    std::size_t outputBytes=0;

    [[noreturn]] static void fail(const char* message) {
        throw std::runtime_error(std::string("Battle-UI updates observer: ")+message);
    }
    void flush() {output.flush();if(!output)fail("trace write failure");}

    static void validateUpdateManager(const BattleUiTrace::Memory& memory,uint32 manager) {
        if(memory.word(manager+0xe40)!=2||memory.word(manager+0xe90)!=0||memory.word(manager+0xe94)!=1)
            fail("manager does not contain the two source-ordered interface layers");
        for(unsigned index=0;index<2;++index) {
            const auto layer=manager+0x40+index*0xe0,record=0x248100+index*112;
            memory.require(layer,0xe0);
            const auto handle=memory.word(layer),type=memory.word(layer+0x60);
            const auto callback=memory.word(layer+0x5c);
            const auto expectedType=index?3u:2u,expectedCallback=index?0x1684d0u:0x164090u;
            if(handle>=8||memory.word(layer+0x54)!=memory.word(record)||type!=expectedType||callback!=expectedCallback)
                fail("manager layer differs from the authored identity/callback");
            const auto window=memory.word(0x200688+handle*4);
            memory.require(window,BattleUiTrace::controllerBytes);
            const auto stream=memory.word(window+0x2698);
            if(memory.word(window+4)!=handle||
               (stream!=memory.word(record+0x54)&&stream!=window+0x26dc)||
               memory.word(window+0x3c00)!=memory.word(record+0x64)||
               memory.word(window+0x3c04)!=memory.word(record+0x68))
                fail("controller window differs from authored identity/capacity or known stream state");
        }
    }

    void snapshot(const char* event,unsigned frame,uint32 pc,uint32 ra,uint32 sp,uint32 fcsr,
                  const BattleUiTrace::Memory& memory,const Call& call,bool post) {
        if(++rows>8)fail("snapshot count exceeds four paired callbacks");
        std::ostringstream row;
        row<<"{\"event\":\""<<event<<"\",\"callback\":\""
           <<(call.kind==0?"0x11fec8":"0x120ad0")<<"\",\"sequence\":"<<rows-1
           <<",\"frame\":"<<frame<<",\"pc\":"<<pc<<",\"ra\":"<<ra<<",\"sp\":"<<sp
           <<",\"fcsr\":"<<fcsr<<",\"root\":"<<call.root<<",\"manager\":"<<call.manager
           <<",\"caller_return_pc\":"<<call.returnPc<<",\"post_callback\":"<<(post?"true":"false");
        const auto camera=memory.word(call.root+0x8068);
        memory.require(camera,0x150);
        memory.bytes(row,"battle_root",call.root,0x81d0);
        memory.bytes(row,"battle_root_81ac",call.root+0x81ac,4);
        row<<",\"camera\":"<<camera;
        memory.bytes(row,"battle_camera",camera,0x150);
        memory.bytes(row,"manager_raw",call.manager,0xef0);
        for(unsigned i=0;i<2;++i)
            memory.bytes(row,i?"layer1":"layer0",call.manager+0x40+i*0xe0,0xe0);
        memory.bytes(row,"authored_interface_records_and_terminator",0x248100,0xe4);
        BattleUiTrace::captureRuw(row,memory);
        BattleUiTrace::captureControllerRegistry(row,memory,true);
        BattleUiTrace::captureAtlas(row,memory,true);
        memory.bytes(row,"stream_type2_written_extent",0x1fd820,20);
        memory.bytes(row,"stream_type3_written_extent",0x1fdb88,20);
        BattleUiTrace::captureGlobals(row,memory);
        row<<",\"party_selection_cache_unchanged_during_callback\":"
           <<(std::memcmp(call.partySelection.data(),memory.ee.m_ram+0x1fae18,
                          call.partySelection.size())==0?"true":"false");
        const uint32 rng=memory.word(0x2151b4);memory.require(rng,0x5c);
        row<<",\"rng_pointer\":"<<rng<<",\"rng_state_word\":"<<memory.word(rng+0x58);
        memory.bytes(row,"rng_state_owner",rng,0x5c);
        captureEmbeddedStreams(row,memory);
        memory.bytes(row,"type2_widget_index_accumulators",0x200a88,64);
        // The command opcodes consume four authored words at each address.
        // Capture those bounded source operands; no larger object extent is claimed.
        memory.bytes(row,"embedded_command_default_0x200b78_words",0x200b78,16);
        memory.bytes(row,"embedded_command_default_0x200b88_words",0x200b88,16);
        row<<",\"pad_globals\":{";
        static constexpr std::array<std::pair<const char*,uint32>,5> pad={{{"0x33958c",0x33958c},
            {"0x339590",0x339590},{"0x1fcb60",0x1fcb60},{"0x1fcb64",0x1fcb64},
            {"0x1fcb68",0x1fcb68}}};
        for(unsigned i=0;i<pad.size();++i) {if(i)row<<',';row<<'"'<<pad[i].first<<"\":"<<memory.word(pad[i].second);}
        row<<"},\"variables\":null,\"variables_note\":\"No variable-owner extent is included; no unproved span is captured.\"}\n";
        const auto text=row.str();
        if(text.size()>512*1024||text.size()>5*1024*1024-outputBytes)
            fail("bounded snapshot output limit exceeded");
        outputBytes+=text.size();output<<text;flush();
    }

    static void captureEmbeddedStreams(std::ostringstream& row,const BattleUiTrace::Memory& memory) {
        const auto raw=memory.word(0x200708);
        const auto widgetBase=memory.word(0x200750+0x24);
        const auto widgetCount=256u;
        const auto widgetBytes=widgetCount*68u;
        memory.require(widgetBase,widgetBytes);
        if(widgetBase<raw||widgetBase-raw>19596u||widgetBytes>19596u-(widgetBase-raw))
            fail("RUW widget records are outside validated raw owner");
        row<<",\"embedded_streams\":[";
        bool first=true;
        for(unsigned slot=0;slot<8;++slot) {
            const auto window=memory.word(0x200688+slot*4);
            if(!window)continue;
            memory.require(window,BattleUiTrace::controllerBytes);
            const auto active=memory.word(window+0x2698);
            const auto embedded=window+0x26dc;
            const auto capacity=memory.word(window+0x3c00);
            if(capacity>32)fail("UI glyph-buffer allocation capacity+1 exceeds source bound 33");
            const auto bufferBytes=(capacity+1)*256;
            if(!first)row<<',';first=false;
            row<<"{\"slot\":"<<slot<<",\"window\":"<<window
               <<",\"active_stream_pointer\":"<<active
               <<",\"embedded_stream_active\":"<<(active==embedded?"true":"false");
            if(active==embedded) {
                const auto cursor=memory.word(window+0x2edc);
                if(cursor>511)fail("embedded UI command cursor exceeds source bound 511");
                const auto count=cursor+1;
                memory.require(embedded,count*4);
                if(count<5||memory.word(embedded)!=0x0e||memory.word(embedded+4)!=0x200b78||
                   memory.word(embedded+8)!=6||memory.word(embedded+12)!=0x200b88||
                   memory.word(embedded+(count-1)*4)!=0)
                    fail("embedded command stream lacks its authored prefix or final terminator");
                row<<",\"cursor\":"<<cursor<<",\"word_count_including_terminator\":"<<count<<",\"words\":[";
                for(uint32 index=0;index<4;++index) {
                    if(index)row<<',';
                    row<<memory.word(embedded+index*4);
                }
                for(uint32 index=4;index<count-1;index+=2) {
                    if(memory.word(embedded+index*4)!=0x17||index+1>=count-1)
                        fail("embedded command stream contains an unknown opcode or incomplete widget pair");
                    const auto pointer=memory.word(embedded+(index+1)*4);
                    if(pointer<widgetBase||pointer-widgetBase>=widgetBytes||(pointer-widgetBase)%68)
                        fail("embedded widget pointer is outside a RUW raw widget record");
                    row<<",23,{\"raw_widget_offset\":"<<(pointer-raw)<<'}';
                }
                row<<",0]";
            }
            row<<",\"glyph_buffer_capacity\":"<<capacity<<",\"glyph_buffer_allocation_count\":"<<capacity+1
               <<",\"glyph_buffer_extent_bytes\":"<<bufferBytes;
            for(const auto& item:std::array<std::pair<const char*,uint32>,2>{{
                    {"glyph_positions",window+0x650},{"glyph_metrics",window+0x654}}}) {
                const auto pointer=memory.word(item.second);
                row<<",\""<<item.first<<"_pointer\":"<<pointer;
                if(pointer&&bufferBytes)memory.bytes(row,item.first,pointer,bufferBytes);
                else row<<",\""<<item.first<<"_bytes\":null";
            }
            row<<'}';
        }
        row<<']';
    }
};
