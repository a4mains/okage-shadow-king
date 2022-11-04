#pragma once

#include "ee/Ee_SubSystem.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

class UiLayoutTrace {
public:
    static constexpr uint32 parentEntry=0x14a248,firstCallback=0x11fec8,
        secondCallback=0x120ad0,dispatchEntry=0x198390,layoutEntry=0x1a1ad8;
    static constexpr std::array<uint32,5> entryBreakpoints={
        parentEntry,firstCallback,secondCallback,dispatchEntry,layoutEntry};
    static constexpr unsigned maxRows=6;
    static constexpr std::size_t maxOutputBytes=16*1024*1024;

    explicit UiLayoutTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))
            fail("refusing existing UI-layout output");
        output.open(path);
        if(!output)fail("cannot create UI-layout output");
        std::ostringstream header;
        header<<"{\"schema\":\"okage-ui-layout-observer-v1\","
              <<"\"scope\":\"first 198390 dispatch after the observed root phase-0-to-1 transition and completed first 11fec8/120ad0 callbacks; bracket the two authored mode-2 windows at 1a1ad8\","
              <<"\"pairing\":\"live RA continuation, restored SP, unchanged root and manager, registry slot/window identity; no frame fitting\","
              <<"\"dispatcher_return\":\"198390 tail-calls 1a8150 when the controller registry is nonempty; dispatcher return is after that rendering stage\","
              <<"\"allocation_extents\":\"window scalar 0x6ec0; flags 0x1000; four glyph/sprite outputs (capacity+1)*0x100; per-group cache pointer arrays capacity*8 and each source allocation 0x12c0; three extra 0x12c0 caches\","
              <<"\"raw_data_limits\":\"Full allocated cache bytes include potentially uninitialized lanes; extents come from 198e38 requested allocations, not adjacent-memory guesses. Captured cache bytes do not claim all bytes are initialized or equivalent. The dispatcher boundary includes 1a8150; no final pixels or rendering equivalence is claimed.\","
              <<"\"stream_cursor\":\"window+0x2edc bounds the embedded stream prefix including terminator (at most 512 words); window+0x269c is captured as scalar state and is not interpreted as the stream word cursor\","
              <<"\"source_elf_sha256\":\"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5\"}\n";
        writeRaw(header.str());
    }

    bool recognizes(uint32 pc) const {
        for(const auto& call:active)if(pc==call.returnPc)return true;
        if(!active.empty()&&isTargetEntry(pc))return true;
        if(complete)return false;
        if(pc==parentEntry)return !parentReady;
        if(pc==firstCallback||pc==secondCallback)return parentReady&&!secondDone;
        if(pc==dispatchEntry)return true;
        return pc==layoutEntry&&resourceActive;
    }

    void observe(Ee::CSubSystem& ee,unsigned frame,std::vector<uint32>& loaders) {
        const auto& cpu=ee.m_EE.m_State;
        const auto pc=uint32(cpu.nPC),sp=uint32(cpu.nGPR[CMIPS::SP].nD0);
        auto reg=[&](unsigned index){return uint32(cpu.nGPR[index].nD0);};
        Memory m{ee};

        if(!active.empty()&&pc==active.back().returnPc) {
            const auto call=active.back();
            validateContinuation(call,reg(CMIPS::RA),sp,m);
            active.pop_back();releaseReturnBreakpoint(ee,loaders,call);
            onReturn(call,frame,pc,reg(CMIPS::RA),sp,cpu.nFCSR,m,ee,loaders);
            return;
        }
        if(std::any_of(active.begin(),active.end(),[&](const Call& c){return pc==c.returnPc;}))
            fail("nested observer continuation returned out of order");
        if(!active.empty()&&isTargetEntry(pc)&&
           !(pc==layoutEntry&&active.back().kind==Kind::Dispatch))
            fail("tracked target entry nested or misordered before its live continuation");

        if(pc==parentEntry) {observeParentEntry(ee,frame,reg(CMIPS::A0),sp,reg(CMIPS::RA),m,loaders);return;}
        if(pc==firstCallback||pc==secondCallback) {
            const auto root=m.word(0x1fce60);if(!root)return;
            const auto manager=m.word(root+0x8060);
            if(!parentReady||!manager||reg(CMIPS::A0)!=manager)return;
            if(!active.empty())fail("UI callback nested inside another tracked target");
            const auto kind=pc==firstCallback?Kind::FirstCallback:Kind::SecondCallback;
            if(kind==Kind::FirstCallback&&(firstDone||secondDone))fail("duplicate first UI callback before selected layout pass");
            if(kind==Kind::SecondCallback&&!firstDone)fail("second UI callback preceded first callback completion");
            if(kind==Kind::SecondCallback&&secondDone)fail("duplicate second UI callback before selected layout pass");
            if(m.word(root+0x24)!=1||root!=candidateRoot)fail("UI callback is outside the observed phase-1 battle root");
            if(kind==Kind::FirstCallback)validateManager(m,root,manager);
            beginCall(ee,loaders,Call{kind,root,manager,0,0,sp,reg(CMIPS::RA),false},pc);
            if(kind==Kind::FirstCallback)++firstEntries;
            else ++secondEntries;
            return;
        }
        if(pc==dispatchEntry) {
            if(!parentReady||!firstDone||!secondDone)return;
            if(resourceActive||complete||!active.empty())fail("another 198390 dispatch began before selected pass completion");
            const auto root=m.word(0x1fce60);
            if(!root||root!=candidateRoot||m.word(root+0x24)!=1)fail("selected dispatcher has no observed phase-1 root");
            const auto manager=m.word(root+0x8060);
            validateManager(m,root,manager);
            resourceActive=true;layoutEntries={};layoutReturns={};
            beginCall(ee,loaders,Call{Kind::Dispatch,root,manager,0,0,sp,reg(CMIPS::RA),false},pc);
            dispatchFrame=frame;
            writeSnapshot("resource_dispatch_entry",frame,pc,reg(CMIPS::RA),sp,cpu.nFCSR,m,root,manager,0,false);
            return;
        }
        if(pc==layoutEntry&&resourceActive) {
            if(active.empty()||active.back().kind!=Kind::Dispatch)fail("mode-2 layout entry is not inside selected 198390 pass");
            const auto root=m.word(0x1fce60);if(root!=candidateRoot)fail("battle root changed during layout pass");
            const auto window=reg(CMIPS::A0);
            const auto manager=m.word(root+0x8060);validateManager(m,root,manager);
            unsigned slot=2;
            for(unsigned i=0;i<2;++i)if(m.word(0x200688+i*4)==window)slot=i;
            if(slot>=2)return;
            if(slot!=layoutEntries[0]||layoutEntries[slot])fail("authored mode-2 windows entered out of slot order or more than once");
            if(m.word(window+0x3bec)!=2)fail("tracked window did not enter source mode-2 layout");
            validateWindowIdentity(m,root,manager,slot,window);
            ++layoutEntries[slot];
            beginCall(ee,loaders,Call{Kind::WindowLayout,root,manager,window,slot,sp,reg(CMIPS::RA),false},pc);
            writeWindowSnapshot("window_mode2_layout_entry",frame,pc,reg(CMIPS::RA),sp,cpu.nFCSR,m,active.back(),false);
            return;
        }
    }

    void finish() {
        if(!complete||resourceActive||!active.empty()||rows!=maxRows||
           !parentReady||!firstDone||!secondDone||layoutEntries[0]!=1||layoutEntries[1]!=1||
           layoutReturns[0]!=1||layoutReturns[1]!=1||firstEntries!=1||secondEntries!=1)
            fail("trace ended before one complete post-callback dispatcher and two mode-2 window pairs");
        std::ostringstream row;
        row<<"{\"event\":\"finish\",\"snapshots\":"<<rows
           <<",\"root_parent_attempts\":"<<parentAttempts
           <<",\"loader_pending_parent_returns\":"<<pendingParentReturns
           <<",\"parent_phase_transition_frame\":"<<parentReadyFrame
           <<",\"first_callback_frame\":"<<firstDoneFrame
           <<",\"second_callback_frame\":"<<secondDoneFrame
           <<",\"resource_dispatch_frame\":"<<dispatchFrame
           <<",\"first_callback_entries\":"<<firstEntries<<",\"second_callback_entries\":"<<secondEntries
           <<",\"window_layout_returns\":[1,1],\"dispatcher_completed\":true}\n";
        writeRaw(row.str());
    }

private:
    enum class Kind {Parent,FirstCallback,SecondCallback,Dispatch,WindowLayout};
    struct Call {Kind kind;uint32 root,manager,window,slot,sp,returnPc;bool ownsBreakpoint;};
    struct Memory {
        Ee::CSubSystem& ee;
        void require(uint32 at,uint32 size) const {
            if(at<0x100000||size>PS2::EE_RAM_SIZE||at>PS2::EE_RAM_SIZE-size)
                fail("snapshot range outside EE RAM");
        }
        uint32 word(uint32 at) const {require(at,4);uint32 value;std::memcpy(&value,ee.m_ram+at,4);return value;}
        void bytes(std::ostringstream& row,const std::string& key,uint32 at,uint32 size) const {
            require(at,size);static constexpr char digits[]="0123456789abcdef";
            row<<",\""<<key<<"\":{\"address\":"<<at<<",\"bytes\":"<<size<<",\"raw_hex\":\"";
            for(uint32 i=0;i<size;++i){const auto b=ee.m_ram[at+i];row<<digits[b>>4]<<digits[b&15];}
            row<<"\"}";
        }
    };
    struct Region {uint32 at,size;std::string label;};
    std::ofstream output;
    std::vector<Call> active;
    std::array<unsigned,2> layoutEntries{},layoutReturns{};
    unsigned parentAttempts=0,pendingParentReturns=0,firstEntries=0,secondEntries=0,rows=0;
    std::size_t outputBytes=0;
    bool parentReady=false,firstDone=false,secondDone=false,resourceActive=false,complete=false;
    uint32 candidateRoot=0,parentReadyFrame=0,firstDoneFrame=0,secondDoneFrame=0,dispatchFrame=0;

    [[noreturn]] static void fail(const char* message) {
        throw std::runtime_error(std::string("UI-layout observer: ")+message);
    }
    static bool isTargetEntry(uint32 pc) {
        return std::find(entryBreakpoints.begin(),entryBreakpoints.end(),pc)!=entryBreakpoints.end();
    }
    void writeRaw(const std::string& text) {
        if(text.empty()||text.size()>8*1024*1024||text.size()>maxOutputBytes-outputBytes)
            fail("bounded output byte limit exceeded");
        outputBytes+=text.size();output<<text;output.flush();if(!output)fail("trace write failed");
    }
    void writeRow(const std::string& text) {
        if(rows>=maxRows)fail("snapshot count exceeded six rows");
        ++rows;writeRaw(text);
    }
    void writeSnapshot(const char* event,unsigned frame,uint32 pc,uint32 ra,uint32 sp,uint32 fcsr,
                       const Memory& m,uint32 root,uint32 manager,unsigned slot,bool post) {
        std::ostringstream row;
        row<<"{\"event\":\""<<event<<"\",\"sequence\":"<<rows
           <<",\"frame\":"<<frame<<",\"pc\":"<<pc<<",\"ra\":"<<ra<<",\"sp\":"<<sp
           <<",\"fcsr\":"<<fcsr<<",\"root\":"<<root<<",\"root_phase\":"<<m.word(root+0x24)
           <<",\"manager\":"<<manager<<",\"slot\":"<<slot<<",\"post\":"<<(post?"true":"false")
           <<",\"gate_frames\":{"<<"\"parent_phase_transition\":"<<parentReadyFrame
           <<",\"first_callback_complete\":"<<firstDoneFrame<<",\"second_callback_complete\":"<<secondDoneFrame<<"}";
        m.bytes(row,"root_header",root,0x81d0);
        m.bytes(row,"manager_raw",manager,0xef0);
        m.bytes(row,"controller_registry",0x200688,32);
        m.bytes(row,"resource_context_200748_200774",0x200748,0x30);
        m.bytes(row,"resource_widget_accumulators_200a88",0x200a88,64);
        row<<",\"shared_layout_words\":{";
        static constexpr std::array<std::pair<const char*,uint32>,5> globals={{{"cursor_200520",0x200520},
            {"layer_20052c",0x20052c},{"dirty_200530",0x200530},{"resource_index_2006ac",0x2006ac},
            {"dispatcher_argument_2005c0",0x2005c0}}};
        for(unsigned i=0;i<globals.size();++i){if(i)row<<',';row<<'\"'<<globals[i].first<<"\":"<<m.word(globals[i].second);}
        row<<"},\"rng_pointer\":";
        const auto rng=m.word(0x2151b4);m.require(rng,0x5c);row<<rng;m.bytes(row,"rng_owner",rng,0x5c);
        const auto text=row.str()+"}\n";
        writeRow(text);
    }
    void writeWindowSnapshot(const char* event,unsigned frame,uint32 pc,uint32 ra,uint32 sp,uint32 fcsr,
                             const Memory& m,const Call& call,bool post) {
        std::ostringstream row;
        const auto window=call.window,slot=call.slot,root=call.root,manager=call.manager;
        row<<"{\"event\":\""<<event<<"\",\"sequence\":"<<rows
           <<",\"frame\":"<<frame<<",\"pc\":"<<pc<<",\"ra\":"<<ra<<",\"sp\":"<<sp
           <<",\"fcsr\":"<<fcsr<<",\"root\":"<<root<<",\"root_phase\":"<<m.word(root+0x24)
           <<",\"manager\":"<<manager<<",\"slot\":"<<slot<<",\"window\":"<<window
           <<",\"layout_mode\":"<<m.word(window+0x3bec)<<",\"post\":"<<(post?"true":"false")
           <<",\"gate_frames\":{"<<"\"parent_phase_transition\":"<<parentReadyFrame
           <<",\"first_callback_complete\":"<<firstDoneFrame<<",\"second_callback_complete\":"<<secondDoneFrame<<"}";
        m.bytes(row,"manager_layer",manager+0x40+slot*0xe0,0xe0);
        m.bytes(row,"window_scalar_owner",window,0x6ec0);
        std::vector<Region> regions{{window,0x6ec0,"window_scalar_owner"}};
        const auto capA=m.word(window+0x3c00),capB=m.word(window+0x3c04);
        if(capA<1||capA>32||capB<1||capB>32)fail("source window capacity outside 1..32");
        allocation(row,m,regions,"flags_allocation",m.word(window+0x18fc),0x1000);
        for(const auto& item:std::array<std::tuple<const char*,uint32,uint32>,4>{{
                {"glyph_positions",0x650,capA},{"glyph_metrics",0x654,capA},
                {"sprite_positions",0x658,capB},{"sprite_metrics",0x65c,capB}}}) {
            const auto* name=std::get<0>(item);const auto offset=std::get<1>(item),capacity=std::get<2>(item);
            allocation(row,m,regions,name,m.word(window+offset),(capacity+1)*0x100);
        }
        captureCacheFamily(row,m,regions,"glyph",m.word(window+0x3d0c),capA);
        captureCacheFamily(row,m,regions,"sprite",m.word(window+0x3d10),capB);
        for(const auto& item:std::array<std::pair<const char*,uint32>,3>{{
                {"extra_cache_0",0x3d14},{"extra_cache_1",0x3d1c},{"extra_cache_2",0x3d20}}})
            allocation(row,m,regions,item.first,m.word(window+item.second),0x12c0);
        const auto stream=m.word(window+0x2698),embedded=window+0x26dc;
        if(stream!=embedded)fail("mode-2 window is not using its source-owned embedded stream");
        const auto cursor=m.word(window+0x2edc);
        if(cursor>511)fail("embedded stream cursor exceeds 511 words");
        const auto count=cursor+1;m.require(embedded,count*4);
        if(m.word(embedded+(count-1)*4)!=0)fail("bounded embedded stream prefix lacks its final zero word");
        row<<",\"embedded_stream_pointer\":"<<stream<<",\"embedded_stream_cursor_2edc\":"<<cursor
           <<",\"embedded_stream_word_count_including_terminator\":"<<count;
        m.bytes(row,"embedded_stream_prefix",embedded,count*4);
        m.bytes(row,"resource_context_200748_200774",0x200748,0x30);
        m.bytes(row,"resource_widget_accumulators_200a88",0x200a88,64);
        row<<",\"shared_layout_words\":{";
        static constexpr std::array<std::pair<const char*,uint32>,5> globals={{{"cursor_200520",0x200520},
            {"layer_20052c",0x20052c},{"dirty_200530",0x200530},{"resource_index_2006ac",0x2006ac},
            {"dispatcher_argument_2005c0",0x2005c0}}};
        for(unsigned i=0;i<globals.size();++i){if(i)row<<',';row<<'\"'<<globals[i].first<<"\":"<<m.word(globals[i].second);}
        row<<"},\"rng_pointer\":";
        const auto rng=m.word(0x2151b4);m.require(rng,0x5c);row<<rng;m.bytes(row,"rng_owner",rng,0x5c);
        writeRow(row.str()+"}\n");
    }
    void allocation(std::ostringstream& row,const Memory& m,std::vector<Region>& regions,
                    const std::string& name,uint32 at,uint32 size) {
        if(!at||(at&3)||!size)fail("missing or unaligned source allocation pointer");
        if(size>PS2::EE_RAM_SIZE||at>PS2::EE_RAM_SIZE-size)fail("source allocation extends outside EE RAM");
        for(const auto& region:regions) {
            const uint64 end=uint64(at)+size,otherEnd=uint64(region.at)+region.size;
            if(uint64(at)<otherEnd&&uint64(region.at)<end)fail("distinct source allocation extents overlap");
        }
        regions.push_back({at,size,name});m.bytes(row,name,at,size);
    }
    void captureCacheFamily(std::ostringstream& row,const Memory& m,std::vector<Region>& regions,
                            const char* family,uint32 table,uint32 count) {
        const auto tableBytes=count*8;
        allocation(row,m,regions,std::string(family)+"_cache_pointer_table",table,tableBytes);
        for(unsigned group=0;group<count;++group)for(unsigned side=0;side<2;++side) {
            const auto pointer=m.word(table+group*8+side*4);
            std::ostringstream name;name<<family<<"_packet_cache_group_"<<group<<"_side_"<<side;
            allocation(row,m,regions,name.str(),pointer,0x12c0);
        }
    }
    void validateManager(const Memory& m,uint32 root,uint32 manager) const {
        if(!manager||m.word(root+0x8060)!=manager||m.word(manager+0xe40)!=2||
           m.word(manager+0xe90)!=0||m.word(manager+0xe94)!=1)
            fail("battle root no longer owns the two source-ordered UI layers");
        for(unsigned slot=0;slot<2;++slot) {
            const auto layer=manager+0x40+slot*0xe0,record=0x248100+slot*112;
            m.require(layer,0xe0);const auto handle=m.word(layer);
            if(handle!=slot||m.word(layer+0x54)!=m.word(record)||
               m.word(layer+0x5c)!=(slot?0x1684d0u:0x164090u)||
               m.word(layer+0x60)!=(slot?3u:2u))
                fail("UI layer identity/callback differs from pinned authored row");
            const auto window=m.word(0x200688+handle*4);m.require(window,0x6ec0);
            if(m.word(window+4)!=handle||m.word(window+0x3c00)!=m.word(record+0x64)||
               m.word(window+0x3c04)!=m.word(record+0x68)||m.word(window+0x2698)!=window+0x26dc)
                fail("UI window handle, capacities, or embedded stream differ from authored layer");
        }
    }
    void validateWindowIdentity(const Memory& m,uint32 root,uint32 manager,unsigned slot,uint32 window) const {
        validateManager(m,root,manager);
        if(m.word(0x200688+slot*4)!=window||m.word(manager+0x40+slot*0xe0)!=slot||
           m.word(window+4)!=slot||m.word(window+0x3bec)!=2)
            fail("mode-2 window does not match source manager/registry slot");
    }
    void observeParentEntry(Ee::CSubSystem& ee,unsigned frame,uint32 a0,uint32 sp,uint32 ra,
                            const Memory& m,std::vector<uint32>& loaders) {
        if(parentReady||active.size())fail("root parent traversal repeated or nested before selected layout");
        const auto root=m.word(0x1fce60);if(!root||a0!=root)return;
        m.require(root,0x81d0);
        if(candidateRoot&&candidateRoot!=root)fail("battle root changed during loader polling");
        if(m.word(root+0x24)!=0)fail("observer reached root parent after phase transition");
        candidateRoot=root;if(++parentAttempts>1024)fail("root parent polling exceeded bound");
        beginCall(ee,loaders,Call{Kind::Parent,root,0,0,0,sp,ra,false},parentEntry);
        (void)frame;
    }
    void beginCall(Ee::CSubSystem& ee,std::vector<uint32>& loaders,Call call,uint32 entry) {
        if(!call.returnPc||(call.returnPc&3)||call.returnPc<0x100000||
           call.returnPc>=PS2::EE_RAM_SIZE||call.returnPc==entry)
            fail("target callback has an invalid live return address");
        for(const auto& existing:active)if(existing.returnPc==call.returnPc)
            fail("nested target calls share an ambiguous live continuation");
        const bool installed=std::find(loaders.begin(),loaders.end(),call.returnPc)!=loaders.end();
        if(!installed){ee.m_EE.ToggleBreakpoint(call.returnPc);loaders.push_back(call.returnPc);}
        call.ownsBreakpoint=!installed;active.push_back(call);
    }
    void validateContinuation(const Call& call,uint32 ra,uint32 sp,const Memory& m) const {
        if(sp!=call.sp||ra!=call.returnPc||m.word(0x1fce60)!=call.root)
            fail("live continuation failed SP/RA/root identity check");
        if(call.kind!=Kind::Parent&&call.kind!=Kind::FirstCallback&&call.kind!=Kind::SecondCallback) {
            if(m.word(call.root+0x8060)!=call.manager)fail("battle UI manager changed during tracked pass");
        }
    }
    void onReturn(const Call& call,unsigned frame,uint32 pc,uint32 ra,uint32 sp,uint32 fcsr,const Memory& m,
                  Ee::CSubSystem& ee,std::vector<uint32>& loaders) {
        if(call.kind==Kind::Parent) {
            const auto phase=m.word(call.root+0x24);
            if(phase==0){if(++pendingParentReturns>1024)fail("loader-pending root return bound exceeded");return;}
            if(phase!=1||parentReady)fail("root parent return did not prove the first phase-0-to-1 transition");
            parentReady=true;parentReadyFrame=frame;return;
        }
        if(call.kind==Kind::FirstCallback) {
            if(m.word(call.root+0x24)!=1||m.word(call.root+0x8060)!=call.manager||firstDone)
                fail("first UI callback return lost source root/manager identity");
            firstDone=true;firstDoneFrame=frame;return;
        }
        if(call.kind==Kind::SecondCallback) {
            if(!firstDone||m.word(call.root+0x24)!=1||m.word(call.root+0x8060)!=call.manager||secondDone)
                fail("second UI callback return lost required first-callback/root/manager identity");
            secondDone=true;secondDoneFrame=frame;return;
        }
        if(call.kind==Kind::WindowLayout) {
            validateWindowIdentity(m,call.root,call.manager,call.slot,call.window);
            if(m.word(call.window+0x3bec)!=2)fail("window left source mode-2 layout during call");
            writeWindowSnapshot("window_mode2_layout_return",frame,pc,ra,sp,fcsr,m,call,true);
            ++layoutReturns[call.slot];return;
        }
        if(call.kind==Kind::Dispatch) {
            if(!resourceActive||layoutEntries[0]!=1||layoutEntries[1]!=1||
               layoutReturns[0]!=1||layoutReturns[1]!=1)
                fail("198390 returned without one ordered mode-2 call for each authored window");
            writeSnapshot("resource_dispatch_return_after_1a8150",frame,pc,ra,sp,fcsr,m,call.root,call.manager,0,true);
            resourceActive=false;complete=true;releaseEntryBreakpoints(ee,loaders);return;
        }
    }
    void releaseReturnBreakpoint(Ee::CSubSystem& ee,std::vector<uint32>& loaders,const Call& call) {
        if(!call.ownsBreakpoint)return;
        ee.m_EE.ToggleBreakpoint(call.returnPc);
        loaders.erase(std::remove(loaders.begin(),loaders.end(),call.returnPc),loaders.end());
    }
    void releaseEntryBreakpoints(Ee::CSubSystem& ee,std::vector<uint32>& loaders) {
        for(const auto at:entryBreakpoints) {
            const auto found=std::find(loaders.begin(),loaders.end(),at);
            if(found==loaders.end())continue;
            ee.m_EE.ToggleBreakpoint(at);loaders.erase(std::remove(loaders.begin(),loaders.end(),at),loaders.end());
        }
    }
};
