#pragma once

#include "ee/Ee_SubSystem.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
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
    static constexpr uint32 drawEntry=0x1a8150;
    static constexpr std::array<uint32,5> entryBreakpoints={
        parentEntry,firstCallback,secondCallback,dispatchEntry,layoutEntry};
    static constexpr std::array<uint32,13> drawBreakpoints={drawEntry,0x1ad1f8,0x1ad850,0x1ad268,
        0x1af978,0x1ad3c8,0x1aeb48,0x1aeb10,0x1ae880,0x1aedd8,0x1ad3c0,0x1c9368,0x1c96c0};
    static constexpr unsigned maxRows=6;
    static constexpr std::size_t maxOutputBytes=16*1024*1024;
    static constexpr std::size_t maxDrawOutputBytes=16*1024*1024;
    static constexpr unsigned maxDrawEvents=20000;

    explicit UiLayoutTrace(const std::filesystem::path& path,
                           const std::filesystem::path& drawPath={}) : drawEnabled(!drawPath.empty()) {
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
        if(drawEnabled) {
            if(std::filesystem::exists(drawPath)||std::filesystem::is_symlink(drawPath))
                fail("refusing existing UI-draw output");
            drawOutput.open(drawPath);
            if(!drawOutput)fail("cannot create UI-draw output");
            drawOutput<<"{\"schema\":\"okage-ui-draw-submission-v1\","
                      <<"\"scope\":\"CPU submission calls inside the first source-gated 1a8150 draw tail-call; no deferred GS vertices or raster output\","
                      <<"\"continuation\":\"198390 tail-calls 1a8150; draw entry and dispatch completion share the original live RA/SP continuation\","
                      <<"\"sources\":{\"projection_words\":32,\"outer_vector_upload_words\":32,\"per_element_vector_upload_words\":16,\"element_row\":\"window+0x3e40+group*0x100+element*0x10, 16 bytes\",\"sprite_owner\":\"window+0x3d10 is the sprite cache pointer table; table[group*8+side*4] selects a 0x12c0 cache\",\"glyph_owner\":\"window+0x3d0c is the glyph cache pointer table; table[group*8+side*4] selects a 0x12c0 cache\"},"
                      <<"\"source_elf_sha256\":\"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5\"}\n";
            drawOutput.flush();if(!drawOutput)fail("UI-draw header write failed");
        }
    }

    bool recognizes(uint32 pc) const {
        if(drawEnabled && resourceActive && pc==drawEntry)return true;
        for(const auto& call:drawCalls)if(pc==call.returnPc)return true;
        if(drawEnabled&&drawActive&&isDrawHelper(pc))return true;
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

        if(drawEnabled&&drawActive&&!drawCalls.empty()&&pc==drawCalls.back().returnPc) {
            const auto call=drawCalls.back();
            validateDrawContinuation(call,reg(CMIPS::RA),sp,m);
            emitDrawCall("call_return",frame,pc,reg(CMIPS::RA),sp,cpu,m,call);
            drawCalls.pop_back();
            if(call.entry==0x1ad3c0)drawAliasPending=false;
            if(call.ownsBreakpoint) {
                ee.m_EE.ToggleBreakpoint(call.returnPc);
                loaders.erase(std::remove(loaders.begin(),loaders.end(),call.returnPc),loaders.end());
            }
            return;
        }
        if(drawEnabled&&drawStarted&&!drawActive&&!active.empty()&&pc==active.front().returnPc) {
            fail("dispatcher continuation reached before the 1a8150 draw entry");
        }
        if(drawEnabled&&resourceActive&&pc==drawEntry) {
            observeDrawEntry(ee,frame,pc,sp,reg(CMIPS::RA),m,loaders);
            return;
        }
        if(drawEnabled&&drawActive&&isDrawHelper(pc)) {
            observeDrawCallEntry(ee,frame,pc,sp,reg(CMIPS::RA),reg,cpu,m,loaders);
            return;
        }

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
        if(drawEnabled) {
            if(!drawCompleted||drawActive||drawStarted==false||!drawCalls.empty()||outerCallIndex!=4)
                fail("draw trace ended before complete 1a8150 CPU submission and shared dispatch continuation");
            drawOutput<<"{\"event\":\"finish\",\"draw_completed\":true,\"helper_calls\":"<<drawHelperCalls
                      <<",\"events\":"<<drawEvents<<",\"bytes\":"<<drawBytesWritten
                      <<",\"gs_vertices_observed\":false}\n";
            drawOutput.flush();if(!drawOutput)fail("UI-draw output flush failed");
        }
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
    struct DrawCall {uint32 id,entry,returnPc,sp,ra,windowSlot,cacheFamily,group,side,element,passIndex,rawPassCounter,passCount,ownerBase,drawType;bool ownsBreakpoint;};
    struct CacheOwner {uint32 window,slot,family,group,side,base;};
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
    std::ofstream drawOutput;
    std::vector<Call> active;
    std::vector<DrawCall> drawCalls;
    std::vector<CacheOwner> spriteOwners;
    std::vector<CacheOwner> glyphOwners;
    std::array<unsigned,2> layoutEntries{},layoutReturns{};
    unsigned parentAttempts=0,pendingParentReturns=0,firstEntries=0,secondEntries=0,rows=0;
    std::size_t outputBytes=0;
    bool parentReady=false,firstDone=false,secondDone=false,resourceActive=false,complete=false;
    uint32 candidateRoot=0,parentReadyFrame=0,firstDoneFrame=0,secondDoneFrame=0,dispatchFrame=0;
    bool drawEnabled=false,drawStarted=false,drawActive=false,drawCompleted=false,drawAliasPending=false;
    unsigned outerCallIndex=0,drawHelperCalls=0,drawEvents=0;
    std::size_t drawBytesWritten=0;
    uint32 drawEntrySp=0,drawReturnPc=0,drawRoot=0,drawManager=0,drawStartFrame=0;

    [[noreturn]] static void fail(const char* message) {
        throw std::runtime_error(std::string("UI-layout observer: ")+message);
    }
    [[noreturn]] static void fail(const std::string& message) {
        throw std::runtime_error(std::string("UI-layout observer: ")+message);
    }
    static bool isTargetEntry(uint32 pc) {
        return std::find(entryBreakpoints.begin(),entryBreakpoints.end(),pc)!=entryBreakpoints.end();
    }
    static bool isDrawHelper(uint32 pc) {
        return std::find(drawBreakpoints.begin()+1,drawBreakpoints.end(),pc)!=drawBreakpoints.end() || pc==0x1c96c0;
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
            if(m.word(window+4)!=handle) {
                std::ostringstream detail;detail<<"UI window handle mismatch at slot "<<slot
                    <<": actual 0x"<<std::hex<<m.word(window+4)<<", expected 0x"<<handle;fail(detail.str());
            }
            if(m.word(window+0x3c00)!=m.word(record+0x64)||m.word(window+0x3c04)!=m.word(record+0x68)) {
                std::ostringstream detail;detail<<"UI window authored capacity mismatch at slot "<<slot
                    <<": actual [0x"<<std::hex<<m.word(window+0x3c00)<<",0x"<<m.word(window+0x3c04)
                    <<"], expected [0x"<<m.word(record+0x64)<<",0x"<<m.word(record+0x68)<<"]";fail(detail.str());
            }
        }
    }
    void validateWindowIdentity(const Memory& m,uint32 root,uint32 manager,unsigned slot,uint32 window) const {
        validateManager(m,root,manager);
        if(m.word(0x200688+slot*4)!=window||m.word(manager+0x40+slot*0xe0)!=slot||
           m.word(window+4)!=slot||m.word(window+0x3bec)!=2)
            fail("mode-2 window does not match source manager/registry slot");
        const auto expected=window+0x26dc,actual=m.word(window+0x2698);
        if(actual!=expected) {
            std::ostringstream detail;detail<<"mode-2 window embedded stream mismatch at slot "<<slot
                <<": actual 0x"<<std::hex<<actual<<", expected 0x"<<expected;fail(detail.str());
        }
    }
    void drawLine(const std::string& text) {
        if(text.empty()||text.size()>1024*1024||drawEvents>=maxDrawEvents||
           text.size()>maxDrawOutputBytes-drawBytesWritten)
            fail("bounded UI-draw event/byte limit exceeded");
        drawBytesWritten+=text.size();++drawEvents;drawOutput<<text<<'\n';
        if(!drawOutput)fail("UI-draw event write failed");
    }
    static std::string hex32(uint32 value) {
        std::ostringstream out;out<<"0x"<<std::hex<<std::setw(8)<<std::setfill('0')<<value;return out.str();
    }
    static std::string hex64(uint64 value) {
        std::ostringstream out;out<<"0x"<<std::hex<<std::setw(16)<<std::setfill('0')<<value;return out.str();
    }
    void buildDrawOwners(const Memory& m,std::ostringstream& row) {
        spriteOwners.clear();glyphOwners.clear();
        row<<",\"windows\":[";
        for(unsigned slot=0;slot<2;++slot) {
            if(slot)row<<',';
            const uint32 window=m.word(0x200688+slot*4);
            validateWindowIdentity(m,drawRoot,drawManager,slot,window);
            const uint32 spriteCapacity=m.word(window+0x3c04),spriteGroups=m.word(window+0xcc4);
            const uint32 glyphCapacity=m.word(window+0x3c00),glyphGroups=m.word(window+0xcc0);
            if(spriteCapacity<1||spriteCapacity>32||spriteGroups>spriteCapacity||
               glyphCapacity<1||glyphCapacity>32)
                fail("draw window cache/group capacity is outside source-owned bounds");
            const uint32 spriteTable=m.word(window+0x3d10);
            const uint32 glyphTable=m.word(window+0x3d0c);
            if(!spriteTable&&spriteGroups)fail("active sprite groups have no source cache pointer table");
            if(!glyphTable&&glyphGroups)fail("enabled glyph draw has no source cache pointer table");
            if(spriteTable)m.require(spriteTable,spriteCapacity*8);
            if(glyphTable)m.require(glyphTable,glyphCapacity*8);
            row<<"{\"slot\":"<<slot<<",\"window\":"<<window
               <<",\"flags_1900\":"<<m.word(window+0x1900)
               <<",\"flags_4f8\":"<<m.word(window+0x4f8)
               <<",\"opacity_270\":"<<m.word(window+0x270)
               <<",\"delta_27c\":"<<m.word(window+0x27c)
               <<",\"framebuffer_target_220\":"<<m.word(window+0x220)
               <<",\"sprite_group_count_cc4\":"<<spriteGroups
               <<",\"glyph_group_count_cc0\":"<<glyphGroups;
            if(spriteTable)m.bytes(row,"sprite_cache_pointer_table",spriteTable,spriteCapacity*8);
            else row<<",\"sprite_cache_pointer_table\":null";
            if(glyphTable)m.bytes(row,"glyph_cache_pointer_table",glyphTable,glyphCapacity*8);
            else row<<",\"glyph_cache_pointer_table\":null";
            if(spriteGroups) {
                m.bytes(row,"sprite_group_flags",window+0x3c08,spriteGroups*4);
                m.bytes(row,"element_color_rows",window+0x3e40,spriteGroups*0x100);
            }
            if(glyphGroups)m.bytes(row,"glyph_group_flags",window+0x3c08,glyphCapacity*4);
            for(unsigned family=0;family<2;++family) {
                const auto capacity=family?spriteCapacity:glyphCapacity;
                const auto table=family?spriteTable:glyphTable;
                auto& owners=family?spriteOwners:glyphOwners;
                for(unsigned group=0;group<capacity;++group)for(unsigned side=0;side<2;++side) {
                    if(!table)continue;
                    const auto base=m.word(table+group*8+side*4);
                    if(base==0)continue;
                    m.require(base,0x12c0);
                    owners.push_back(CacheOwner{window,slot,family,group,side,base});
                }
            }
            row<<'}';
        }
        row<<']';
    }
    void observeDrawEntry(Ee::CSubSystem& ee,unsigned frame,uint32 pc,uint32 sp,uint32 ra,const Memory& m,
                          std::vector<uint32>& loaders) {
        if(drawStarted||!resourceActive||active.empty()||active.back().kind!=Kind::Dispatch||
           layoutEntries[0]!=1||layoutEntries[1]!=1||layoutReturns[0]!=1||layoutReturns[1]!=1)
            fail("1a8150 entered outside the selected completed mode-2 dispatch");
        const auto& dispatch=active.back();
        if(ra!=dispatch.returnPc||sp!=dispatch.sp||m.word(0x1fce60)!=dispatch.root||
           m.word(dispatch.root+0x8060)!=dispatch.manager)
            fail("1a8150 did not preserve the 198390 live continuation/root/manager");
        for(std::size_t i=1;i<drawBreakpoints.size();++i) {
            const auto at=drawBreakpoints[i];
            if(std::find(loaders.begin(),loaders.end(),at)!=loaders.end())
                fail("source-gated draw helper breakpoint conflicts with another observer return");
            ee.m_EE.ToggleBreakpoint(at);loaders.push_back(at);
        }
        drawStarted=drawActive=true;drawEntrySp=sp;drawReturnPc=ra;drawRoot=dispatch.root;
        drawManager=dispatch.manager;drawStartFrame=frame;
        std::ostringstream row;
        row<<"{\"event\":\"draw_submission_entry\",\"frame\":"<<frame<<",\"pc\":"<<pc
           <<",\"ra\":"<<ra<<",\"sp\":"<<sp<<",\"shared_dispatch_return\":"<<ra
           <<",\"root\":"<<drawRoot<<",\"manager\":"<<drawManager
           <<",\"root_phase\":"<<m.word(drawRoot+0x24);
        m.bytes(row,"projection_input_200540",0x200540,128);
        row<<",\"history_counters\":{\"200b58\":"<<m.word(0x200b58)
           <<",\"200b5c\":"<<m.word(0x200b5c)<<'}';
        buildDrawOwners(m,row);
        drawLine(row.str()+"}");
    }
    void emitDrawCall(const char* event,unsigned frame,uint32 pc,uint32 ra,uint32 sp,
                      const auto& cpu,const Memory& m,const DrawCall& call) {
        std::ostringstream row;
        row<<"{\"event\":\""<<event<<"\",\"call_id\":"<<call.id
           <<",\"callee\":"<<hex32(call.entry)<<",\"frame\":"<<frame<<",\"pc\":"<<pc
           <<",\"ra\":"<<ra<<",\"sp\":"<<sp<<",\"callsite\":"<<hex32(call.ra-8)
           <<",\"root\":"<<drawRoot<<",\"manager\":"<<drawManager;
        if(std::strcmp(event,"call_entry")==0) {
            row<<",\"a0\":"<<uint32(cpu.nGPR[CMIPS::A0].nD0)
               <<",\"a1\":"<<uint32(cpu.nGPR[CMIPS::A1].nD0)
               <<",\"a2\":"<<uint32(cpu.nGPR[CMIPS::A2].nD0)
               <<",\"a3\":"<<uint32(cpu.nGPR[CMIPS::A3].nD0);
            if(call.entry==0x1aedd8) {
                row<<",\"gs_register\":"<<uint32(cpu.nGPR[CMIPS::A0].nD0)
                   <<",\"gs_value_a1_u64\":"<<hex64(uint64(cpu.nGPR[CMIPS::A1].nD0));
            }
            if(call.entry==0x1c96c0) {
                row<<",\"queue_arg_a0_u64\":"<<hex64(uint64(cpu.nGPR[CMIPS::A0].nD0))
                   <<",\"queue_arg_a1_u64\":"<<hex64(uint64(cpu.nGPR[CMIPS::A1].nD0));
            }
            if(call.entry==0x1aeb48&&call.windowSlot<2) {
                const auto window=m.word(0x200688+call.windowSlot*4);
                const auto at=window+0x3e40+call.group*0x100+call.element*0x10;
                m.bytes(row,"selected_element_color_row",at,16);
            }
            if(call.entry==0x1ae880) {
                row<<",\"f12_f15_bits\":[";
                for(unsigned i=0;i<4;++i){if(i)row<<',';row<<cpu.nCOP1[12+i];}
                row<<']';
                if(call.windowSlot<2) {
                    const auto window=m.word(0x200688+call.windowSlot*4);
                    const auto group=call.group,element=call.element;
                    const uint32 at=window+0x3e40+group*0x100+element*0x10;
                    m.bytes(row,"element_color_row",at,16);
                }
            }
            if(call.entry==0x1ad850)m.bytes(row,"outer_projection_vectors",uint32(cpu.nGPR[CMIPS::A0].nD0),128);
            if(call.entry==0x1ad3c8)m.bytes(row,"vector_upload",uint32(cpu.nGPR[CMIPS::A0].nD0),64);
            if(call.windowSlot<2&&(call.entry==0x1ad3c0||call.entry==0x1c9368)) {
                row<<",\"packet_owner\":{\"window_slot\":"<<call.windowSlot
                   <<",\"family\":\""<<(call.cacheFamily?"sprite":"glyph")<<"\",\"draw_type\":\""
                   <<(call.drawType==1?"sprite_header":call.drawType==2?"sprite_element":"glyph_pass")
                   <<"\",\"group\":"<<call.group<<",\"side\":"<<call.side
                   <<",\"element\":";
                if(call.cacheFamily)row<<call.element;else row<<"null";
                row<<",\"pass_index\":";
                row<<call.passIndex;
                row<<",\"raw_pass_counter\":"<<call.rawPassCounter
                   <<",\"pass_count\":"<<call.passCount<<",\"cache_base\":"<<hex32(call.ownerBase)
                   <<",\"cache_offset\":"<<hex32(uint32(cpu.nGPR[CMIPS::A0].nD0)-call.ownerBase)<<'}';
            }
        }
        drawLine(row.str()+"}");
    }
    void validateDrawContinuation(const DrawCall& call,uint32 ra,uint32 sp,const Memory& m) const {
        if(sp!=call.sp||ra!=call.returnPc||m.word(0x1fce60)!=drawRoot||
           m.word(drawRoot+0x8060)!=drawManager||!drawActive)
            fail("UI-draw helper did not restore its live SP/RA/root/manager");
    }
    void observeDrawCallEntry(Ee::CSubSystem& ee,unsigned frame,uint32 pc,uint32 sp,uint32 ra,
                              const auto& reg,const auto& cpu,const Memory& m,std::vector<uint32>& loaders) {
        if(drawCalls.size()>=32||drawHelperCalls>=10000)fail("draw helper nesting/call bound exceeded");
        if(pc==0x1c9368&&drawAliasPending&&!drawCalls.empty()&&
           drawCalls.back().entry==0x1ad3c0&&ra==drawCalls.back().returnPc) {
            if(sp!=drawCalls.back().sp)fail("1ad3c0 direct jump changed source stack pointer before its thunk");
            std::ostringstream row;row<<"{\"event\":\"thunk_alias_entry\",\"frame\":"<<frame
                <<",\"pc\":"<<pc<<",\"ra\":"<<ra<<",\"sp\":"<<sp
                <<",\"call_id\":"<<drawCalls.back().id<<",\"aliases\":"<<hex32(0x1ad3c0);
            drawAliasPending=false;drawLine(row.str()+"}");return;
        }
        if(pc==0x1c9368&&drawAliasPending)drawAliasPending=false;
        static constexpr std::array<uint32,4> outer={0x1ad1f8,0x1ad850,0x1ad268,0x1af978};
        if(outerCallIndex<outer.size()) {
            static constexpr std::array<uint32,4> outerCallsites={0x1a81ec,0x1a81f8,0x1a8200,0x1a8208};
            if(pc!=outer[outerCallIndex])fail("outer matrix/GS setup calls were not observed in source order");
            if(ra-8!=outerCallsites[outerCallIndex])fail("outer matrix/GS setup came from an unknown source callsite");
            if(pc==0x1ad850&&reg(CMIPS::A0)!=0x200540)fail("1ad850 projection input is not the source 200540 array");
            if(pc==0x1ad268&&reg(CMIPS::A0)!=0)fail("1ad268 outer reset argument is not zero");
            ++outerCallIndex;
        }
        DrawCall call{};call.id=++drawHelperCalls;call.entry=pc;call.returnPc=ra;call.sp=sp;call.ra=ra;
        call.windowSlot=2;call.cacheFamily=call.group=call.side=call.element=call.passIndex=call.rawPassCounter=0;
        call.passCount=call.ownerBase=call.drawType=0;
        call.ownsBreakpoint=false;
        if(!ra||ra<0x100000||ra>=PS2::EE_RAM_SIZE||(ra&3)||ra==pc)
            fail("draw helper has invalid live return address");
        if(pc==0x1ad3c8) {
            const uint32 site=ra-8;
            if(site!=0x1a8750&&site!=0x1a8b0c&&site!=0x1a8d9c)
                fail("1ad3c8 upload came from an unknown source callsite");
            const uint32 expected=site==0x1a8b0c?sp+0x40:sp;
            if(reg(CMIPS::A0)!=expected)fail("1ad3c8 upload pointer differs from source temporary");
        }
        if(pc==0x1aeb48||(pc==0x1ae880&&(ra-8==0x1a8914||ra-8==0x1a8948))) {
            const uint32 window=reg(CMIPS::S0),group=reg(CMIPS::S1);
            unsigned slot=2;for(unsigned i=0;i<2;++i)if(m.word(0x200688+i*4)==window)slot=i;
            if(slot>=2)fail("element draw helper does not belong to one of the two source windows");
            const uint32 groupOffset=reg(CMIPS::S3),elementOffset=reg(CMIPS::S4);
            if(groupOffset%0x100||elementOffset%0x10||groupOffset/0x100!=group||elementOffset/0x10>=16)
                fail("sprite group/element registers do not match the source loop");
            call.windowSlot=slot;call.group=group;call.element=elementOffset/0x10;
            const auto capacity=m.word(window+0x3c04),groups=m.word(window+0xcc4);
            if(group>=capacity||group>=groups)fail("source sprite group exceeds captured window bounds");
            const uint32 rowAt=window+0x3e40+group*0x100+call.element*0x10;
            m.require(rowAt,16);
            if(pc==0x1aeb48) {
                if(ra-8!=0x1a88a0||reg(CMIPS::A0)!=call.element+5||reg(CMIPS::A1)!=1)
                    fail("per-element color selector differs from source 1aeb48 call");
            }
            if(pc==0x1ae880) {
                const auto site=ra-8;
                if(site==0x1a8914||site==0x1a8948) {
                    for(unsigned i=0;i<3;++i)if(cpu.nCOP1[12+i]!=m.word(rowAt+i*4))
                        fail("per-element F12-F14 color does not match its source color row");
                }
            }
        }
        if(pc==0x1ad3c0||pc==0x1c9368) {
            const auto site=ra-8;
            const uint32 window=reg(CMIPS::S0),group=reg(CMIPS::S1),packet=reg(CMIPS::A0);
            unsigned slot=2;for(unsigned i=0;i<2;++i)if(m.word(0x200688+i*4)==window)slot=i;
            if(slot>=2)fail("packet replay owner is not one of the selected source windows");
            call.windowSlot=slot;call.group=group;
            const CacheOwner* owner=nullptr;
            uint32 expected=0;
            if(site==0x1a8894||site==0x1a8b30) {
                const uint32 groupOffset=reg(CMIPS::S3),elementOffset=reg(CMIPS::S4);
                if(groupOffset%0x100||elementOffset%0x10||groupOffset/0x100!=group||elementOffset/0x10>=16||
                   m.word(sp+0x114)!=window+0x5ec0||m.word(sp+0x134)!=group*0x40||
                   m.word(sp+0x108)!=(elementOffset/0x10)*4)
                    fail("sprite packet source stack selectors differ from recovered loop");
                const uint32 sideOffset=m.word(sp+0x120);
                if(sideOffset!=0&&sideOffset!=0x800)fail("sprite cache side is outside source 0/1");
                call.cacheFamily=1;call.side=sideOffset>>11;call.element=elementOffset/0x10;
                call.rawPassCounter=reg(CMIPS::S2);call.passCount=m.word(sp+0x10c)+1;
                const uint32 expectedPassCount=(m.word(window+0x4f8)&0x40)?3:1;
                if(call.passCount!=expectedPassCount)
                    fail("sprite replay pass selector differs from the source icon-extra flag");
                if(site==0x1a8894) {
                    call.drawType=1;
                    call.passIndex=call.rawPassCounter;
                    if(call.rawPassCounter>=call.passCount)
                        fail("sprite header replay counter exceeds the source pass loop");
                } else {
                    call.drawType=2;
                    if(call.rawPassCounter==0||call.rawPassCounter>call.passCount)
                        fail("sprite element replay counter is outside the source pass loop");
                    call.passIndex=call.rawPassCounter-1;
                }
                if(group>=m.word(window+0x3c04)||group>=m.word(window+0xcc4))
                    fail("sprite packet group exceeds source window bounds");
                expected=m.word(m.word(window+0x3d10)+group*8+call.side*4);
                if(site==0x1a8b30) {
                    const uint32 selectorAddress=m.word(sp+0x114)+m.word(sp+0x134)+sideOffset+m.word(sp+0x108);
                    const auto selectedSubpacket=m.word(selectorAddress);
                    if(packet!=selectedSubpacket||uint64(packet)<expected||uint64(packet)>=uint64(expected)+0x12c0)
                        fail("sprite element packet differs from the selected source subpacket/cache owner");
                }
                if(site==0x1a8894&&packet!=expected)
                    fail("sprite header packet differs from the selected cache-table row");
            } else if(site==0x1a8f20) {
                call.cacheFamily=0;call.drawType=3;call.side=m.word(window+0x268c)?1:0;
                call.rawPassCounter=m.word(sp+0xf8);call.passIndex=call.rawPassCounter-1;call.passCount=m.word(sp+0xf0);
                const auto capacity=m.word(window+0x3c00),groupCount=m.word(window+0xcc0);
                if(call.passCount<1||call.passCount>2||call.rawPassCounter<1||call.rawPassCounter>call.passCount||
                   group>=capacity||groupCount==0)
                    fail("glyph cache group/side/pass is outside source-owned bounds");
                call.element=call.passIndex;
                expected=m.word(m.word(window+0x3d0c)+group*8+call.side*4);
            } else fail("packet replay came from an unknown source callsite");
            for(const auto& candidate:(call.cacheFamily?spriteOwners:glyphOwners)) {
                if(candidate.window==window&&candidate.slot==slot&&candidate.family==call.cacheFamily&&
                   candidate.group==group&&candidate.side==call.side&&candidate.base==expected) {
                    if(owner)fail("selected cache-table row maps to duplicate owner records");
                    owner=&candidate;
                }
            }
            if(!expected||!owner||(site==0x1a8f20&&packet!=expected))
                fail("packet pointer differs from its exact selected window/cache-family/slot/group/side row");
            call.ownerBase=owner->base;
        }
        if(pc==0x1aeb10) {
            const auto site=ra-8;
            if(site!=0x1a859c&&site!=0x1a8bb4)fail("auxiliary color upload came from an unknown source callsite");
            const uint32 expectedA1=site==0x1a859c?1:2;
            if(reg(CMIPS::A0)!=0||reg(CMIPS::A1)!=expectedA1)
                fail("auxiliary color upload arguments differ from the source branch");
        }
        if(pc==0x1ae880) {
            const auto site=ra-8;
            if(site!=0x1a8914&&site!=0x1a8948&&site!=0x1a85b0&&site!=0x1a8bc8&&site!=0x1a8bdc)
                fail("1ae880 color upload came from an unknown source callsite");
        }
        if(pc==0x1c96c0&&(((ra-8)!=0x1a9024&&(ra-8)!=0x1a90f0)||reg(CMIPS::A0)!=m.word(0x2005c0)))
            fail("packet queue operation differs from the two source 1a8150 append sites");
        if(pc==0x1aedd8&&outerCallIndex!=outer.size())fail("GS register writes began before outer setup completed");
        if(pc==0x1aedd8) {
            static constexpr std::array<uint32,10> callers={0x1a8214,0x1a8224,0x1a8238,0x1a8244,0x1a8250,
                0x1a8268,0x1a8274,0x1a8280,0x1a8380,0x1a8ffc};
            if(std::find(callers.begin(),callers.end(),ra-8)==callers.end())
                fail("GS register write came from an unknown source callsite");
        }
        if(pc==0x1aeb10&&outerCallIndex!=outer.size())fail("auxiliary uploads began before outer setup completed");
        if(pc==0x1ad3c8&&outerCallIndex!=outer.size())fail("per-window vector upload preceded outer setup completion");
        const bool installed=std::find(loaders.begin(),loaders.end(),ra)!=loaders.end();
        if(ra==drawReturnPc||installed)fail("draw helper return breakpoint aliases another active continuation");
        if(!installed){ee.m_EE.ToggleBreakpoint(ra);loaders.push_back(ra);call.ownsBreakpoint=true;}
        drawCalls.push_back(call);
        emitDrawCall("call_entry",frame,pc,ra,sp,cpu,m,drawCalls.back());
        if(pc==0x1ad3c0)drawAliasPending=true;
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
            if(drawEnabled) {
                if(!drawStarted||!drawActive||!drawCalls.empty()||outerCallIndex!=4||
                   ra!=drawReturnPc||sp!=drawEntrySp||call.returnPc!=drawReturnPc||call.sp!=drawEntrySp||
                   call.root!=drawRoot||m.word(call.root+0x8060)!=drawManager)
                    fail("shared draw/198390 continuation failed source identity or helper-order checks");
                std::ostringstream drawRow;
                drawRow<<"{\"event\":\"draw_dispatch_completion\",\"frame\":"<<frame
                       <<",\"pc\":"<<pc<<",\"ra\":"<<ra<<",\"sp\":"<<sp
                       <<",\"shared_return\":"<<drawReturnPc<<",\"root\":"<<drawRoot
                       <<",\"manager\":"<<drawManager<<",\"draw_entry_frame\":"<<drawStartFrame
                       <<",\"outer_setup_calls\":"<<outerCallIndex<<",\"helper_calls\":"<<drawHelperCalls
                       <<",\"layout_pairs\":[1,1],\"deferred_gs_consumption\":true,\"gs_vertices_observed\":false}";
                drawLine(drawRow.str());drawActive=false;drawCompleted=true;
            }
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
        if(drawEnabled)for(const auto at:drawBreakpoints) {
            const auto found=std::find(loaders.begin(),loaders.end(),at);
            if(found==loaders.end())continue;
            ee.m_EE.ToggleBreakpoint(at);loaders.erase(std::remove(loaders.begin(),loaders.end(),at),loaders.end());
        }
    }
};
