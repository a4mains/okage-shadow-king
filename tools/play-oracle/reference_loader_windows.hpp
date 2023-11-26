#pragma once

#include "ee/Ee_SubSystem.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

class LoaderWindowsTrace {
public:
    static constexpr uint32 loadWindow=0x178b50;
    static constexpr uint32 windowResumeCall=0x178bfc;
    static constexpr uint32 windowAfterResume=0x178c04;
    static constexpr uint32 windowBeforeSuspend=0x178c10;
    static constexpr uint32 windowAfterSuspend=0x178c18;
    static constexpr uint32 dmaBoundary=0x178310;
    static constexpr uint32 vblankBoundary=0x179388;
    static constexpr uint32 loadCallback=0x14a118;
    static constexpr uint32 stageConstructor=0x152220;
    static constexpr uint32 cameraConstructor=0x14ef78;
    static constexpr uint32 partyConstructor=0x152378;
    static constexpr uint32 enemyConstructor=0x15ae60;
    static constexpr uint32 rngEntry=0x1f14b0;
    static constexpr uint32 stageReturn=0x14a200;
    static constexpr uint32 cameraReturn=0x14a20c;
    static constexpr std::array<uint32,14> entryBreakpoints={
        dmaBoundary,vblankBoundary,loadWindow,loadCallback,
        stageConstructor,cameraConstructor,partyConstructor,enemyConstructor,
        stageReturn,cameraReturn,
        windowResumeCall,windowAfterResume,windowBeforeSuspend,windowAfterSuspend
    };
    static constexpr unsigned maxEvents=200'000;
    static constexpr unsigned maxCalls=50'000;
    static constexpr std::size_t maxOutputBytes=128u*1024u*1024u;

    explicit LoaderWindowsTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))
            fail("refusing existing or symlink output");
        output_.open(path);
        if(!output_)fail("cannot create output");
        output_ << "{\"schema\":\"okage-loader-windows-v1\","
                << "\"format\":\"JSONL; decimal addresses/words; raw bytes are lowercase ascending-address hex strings\","
                << "\"source_elf_sha256\":\"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5\","
                << "\"source_evidence_sha256\":{\"178b50_asm\":\"7a51e27c0640b12da112138921bbf5bd4a788d84f131b3c2b31749a36cefdebb\",\"14a118_asm\":\"02762d356a9e2bbd57dd685435828795f45cf437748d271ad5820fb9bdd9eb4b\"},"
                << "\"scope\":\"natural entries/returns at 178310, 179388 and 178b50 including its main-thread resume/wait/suspend boundaries; 14a118 loader callback with stage/camera/party/enemy constructor entries and returns plus caller continuations at 14a200 and 14a20c; shared RNG entries while any 14a118 callback is active\","
                << "\"pairing\":\"observed calls pair by same guest thread, live RA continuation and restored entry SP. Loaded-state prefix boundaries that lack an observed entry are explicitly unpaired; frame is recorded only and never selects a sample\","
                << "\"raw_context\":\"thread IDs, semaphore handles, queue entries, predicate/payload, counters and RNG state are read-only source globals; values are not interpreted as host handles or readiness\","
                << "\"semantics_limits\":\"No thread/semaphore syscalls are invoked. 178b50 observations retain current/main/loader thread IDs but do not infer which branch ran. DMA registers outside EE RAM are not sampled. Child continuation labels indicate source return/store boundaries, not a claim that constructors are scheduler-atomic. A zero observed count is not proof that the source call was absent.\","
                << "\"bounds\":{\"events\":"<<maxEvents<<",\"calls\":"<<maxCalls
                <<",\"event_bytes\":262144,\"total_bytes\":"<<maxOutputBytes<<"}}\n";
        flush();
    }

    bool recognizes(uint32 pc) const {
        if(std::find(entryBreakpoints.begin(),entryBreakpoints.end(),pc)!=entryBreakpoints.end())return true;
        if(pc==rngEntry)return std::any_of(calls_.begin(),calls_.end(),[](const Call& c){return c.kind==Kind::LoadCallback;});
        return std::any_of(calls_.begin(),calls_.end(),[&](const Call& c){return c.returnPc==pc;});
    }

    void observe(Ee::CSubSystem& ee,unsigned frame,std::vector<uint32>& breakpoints) {
        const auto& cpu=ee.m_EE.m_State;
        const auto reg=[&](unsigned index){return uint32(cpu.nGPR[index].nD0);};
        const auto pc=uint32(cpu.nPC),sp=reg(CMIPS::SP),thread=Memory{ee}.thread();
        Memory m{ee};
        std::optional<std::size_t> returned;
        for(std::size_t i=calls_.size();i>0;--i) {
            const auto& c=calls_[i-1];
            if(c.thread==thread&&c.returnPc==pc&&c.entrySp==sp) {
                if(returned)fail("ambiguous same-thread RA/SP continuation");
                returned=i-1;
            }
        }
        if(returned) {
            const Call c=calls_[*returned];
            if(reg(CMIPS::RA)!=c.returnPc)fail("continuation did not restore live RA");
            emit("call_return",c.kind,c.id,frame,pc,reg(CMIPS::RA),sp,thread,c.entryPc,
                 c.entryFrame,c.a0,c.a1,c.a2,c.a3,m,c.owner,
                 reg(CMIPS::V0),reg(CMIPS::S0),reg(CMIPS::S1),0);
            calls_.erase(calls_.begin()+std::ptrdiff_t(*returned));
            releaseDynamicBreakpoint(ee,breakpoints,c.returnPc);
            if(c.kind==Kind::LoadCallback)releaseRngBreakpointIfIdle(ee,breakpoints);
        }

        if(pc==windowResumeCall||pc==windowAfterResume||pc==windowBeforeSuspend||pc==windowAfterSuspend) {
            const auto call=std::find_if(calls_.rbegin(),calls_.rend(),[&](const Call& c){
                return c.kind==Kind::LoadWindow&&c.thread==thread;
            });
            const auto mainThread=m.word(0x31fa4c);
            if(thread!=mainThread)fail("178b50 main-window inner boundary reached by a non-main thread");
            const char* label=pc==windowResumeCall?"resume_call_entry_before_delay_slot":
                              pc==windowAfterResume?"resume_return_before_wait_sema":
                              pc==windowBeforeSuspend?"wait_sema_return_before_suspend_call":
                              "suspend_return_before_active_clear";
            if(call==calls_.rend()) {
                const std::string event=std::string("unpaired_loaded_prefix_")+label;
                emitUnpairedBoundary(event.c_str(),Kind::LoadWindow,frame,pc,reg(CMIPS::RA),sp,thread,
                    reg(CMIPS::A0),reg(CMIPS::A1),reg(CMIPS::A2),reg(CMIPS::A3),reg(CMIPS::V0),
                    reg(CMIPS::S0),reg(CMIPS::S1),m);
            } else {
                if(call->entrySp<0x30||sp!=call->entrySp-0x30)
                    fail("178b50 inner boundary stack does not match source frame");
                emit(label,Kind::LoadWindow,call->id,frame,pc,reg(CMIPS::RA),sp,thread,
                     call->entryPc,call->entryFrame,reg(CMIPS::A0),reg(CMIPS::A1),reg(CMIPS::A2),
                     reg(CMIPS::A3),m,0,reg(CMIPS::V0),reg(CMIPS::S0),reg(CMIPS::S1),0);
            }
            return;
        }

        if(pc==stageReturn||pc==cameraReturn) {
            const auto* callback=activeLoaderCallback(thread);
            if(callback) {
                if(callback->entrySp<0x50||sp!=callback->entrySp-0x50||reg(CMIPS::S1)!=callback->owner)
                    fail("14a118 child continuation lost its source stack/root identity");
                emitBoundary(pc==stageReturn?"stage_return_continuation":"camera_return_before_parent_store",
                    frame,pc,reg(CMIPS::RA),sp,thread,reg(CMIPS::A0),reg(CMIPS::A1),reg(CMIPS::A2),
                    reg(CMIPS::A3),reg(CMIPS::V0),reg(CMIPS::S0),reg(CMIPS::S1),m);
            } else {
                emitUnpairedBoundary("unpaired_loaded_prefix_child_continuation",Kind::LoadCallback,
                    frame,pc,reg(CMIPS::RA),sp,thread,reg(CMIPS::A0),reg(CMIPS::A1),reg(CMIPS::A2),
                    reg(CMIPS::A3),reg(CMIPS::V0),reg(CMIPS::S0),reg(CMIPS::S1),m);
            }
            return;
        }
        if(pc==rngEntry) {
            std::vector<const Call*> callbacks;
            for(const auto& call:calls_)if(call.kind==Kind::LoadCallback)callbacks.push_back(&call);
            if(!callbacks.empty()) {
                ++kindCounts_[std::size_t(Kind::Rng)];
                emit("rng_entry_while_loader_callback_active",Kind::Rng,0,frame,pc,reg(CMIPS::RA),sp,thread,
                    0,frame,reg(CMIPS::A0),reg(CMIPS::A1),reg(CMIPS::A2),reg(CMIPS::A3),m,0,
                    reg(CMIPS::V0),reg(CMIPS::S0),reg(CMIPS::S1),++rngObservations_,&callbacks);
            }
            return;
        }

        Kind kind;
        if(pc==dmaBoundary)kind=Kind::DmaBoundary;
        else if(pc==vblankBoundary)kind=Kind::VblankBoundary;
        else if(pc==loadWindow)kind=Kind::LoadWindow;
        else if(pc==loadCallback)kind=Kind::LoadCallback;
        else if(pc==stageConstructor)kind=Kind::StageConstructor;
        else if(pc==cameraConstructor)kind=Kind::CameraConstructor;
        else if(pc==partyConstructor)kind=Kind::PartyConstructor;
        else if(pc==enemyConstructor)kind=Kind::EnemyConstructor;
        else return;
        const auto* activeCallback=isChildConstructor(kind)?activeLoaderCallback(thread):nullptr;

        if(calls_.size()>=16)fail("simultaneous observed call bound exceeded");
        if(++callsStarted_>maxCalls)fail("call count bound exceeded");
        const auto ra=reg(CMIPS::RA);
        if((ra&3)||ra<0x100000||ra>=PS2::EE_RAM_SIZE||ra==pc)
            fail("invalid live return address at observed entry");
        const auto callId=callsStarted_;
        uint32 owner=0;
        if(kind==Kind::LoadCallback) {
            if(thread!=m.word(0x31fa50))fail("14a118 callback did not run on the registered loader thread");
            const auto readIndex=m.word(0x1fe248),writeIndex=m.word(0x1fe244);
            if(readIndex>=32||writeIndex>=32||readIndex==writeIndex)
                fail("14a118 callback has invalid or empty loader-queue indices");
            const auto payload=reg(CMIPS::A0);
            const auto slot=0x31faf0+readIndex*16;
            if(payload!=slot+4||m.word(slot)!=loadCallback)
                fail("14a118 callback argument does not match the live loader queue slot");
            m.require(payload,4);
            owner=m.word(payload);
            m.require(owner,0x8090); // source reads through owner+0x8088.
        } else if(activeCallback) {
            owner=activeCallback->owner;
            if(reg(CMIPS::A0)!=owner)fail("loader constructor A0 does not match 14a118 root");
        }
        acquireDynamicBreakpoint(ee,breakpoints,ra);
        if(kind==Kind::LoadCallback)acquireRngBreakpoint(ee,breakpoints);
        Call c{kind,callId,thread,sp,ra,pc,frame,reg(CMIPS::A0),reg(CMIPS::A1),
               reg(CMIPS::A2),reg(CMIPS::A3),owner};
        calls_.push_back(c);
        ++kindCounts_[std::size_t(kind)];
        emit(isChildConstructor(kind)?(owner?"constructor_entry":"unpaired_constructor_entry"):"call_entry",kind,callId,frame,pc,ra,sp,thread,pc,frame,c.a0,c.a1,c.a2,c.a3,m,owner,
             reg(CMIPS::V0),reg(CMIPS::S0),reg(CMIPS::S1),0);
    }

    void finish(unsigned completedFrames) {
        output_<<"{\"event\":\"finish\",\"events\":"<<events_
               <<",\"calls_started\":"<<callsStarted_<<",\"calls_completed\":"<<(callsStarted_-calls_.size())
               <<",\"observed_call_entries_and_rng_entries\":{";
        for(std::size_t i=0;i<kindCounts_.size();++i) {
            if(i)output_<<',';
            output_<<'\"'<<name(Kind(i))<<"\":"<<kindCounts_[i];
        }
        output_<<'}'<<",\"pending_calls\":[";
        bool comma=false;
        for(const auto& call:calls_) {
            if(comma)output_<<',';comma=true;
            output_<<"{\"id\":"<<call.id<<",\"kind\":\""<<name(call.kind)
                   <<"\",\"thread_id\":"<<call.thread<<",\"entry_pc\":"<<call.entryPc
                   <<",\"return_pc\":"<<call.returnPc<<",\"entry_sp\":"<<call.entrySp<<'}';
        }
        output_<<"],\"completed_vblanks\":"<<completedFrames
               <<",\"balanced_observed_calls\":"<<(calls_.empty()?"true":"false")
               <<",\"complete\":"<<(calls_.empty()?"true":"false")<<"}\n";
        flush();
    }

private:
    enum class Kind {DmaBoundary,VblankBoundary,LoadWindow,LoadCallback,StageConstructor,CameraConstructor,PartyConstructor,EnemyConstructor,Rng};
    struct Call {
        Kind kind;uint32 id,thread,entrySp,returnPc,entryPc,entryFrame;
        uint32 a0,a1,a2,a3,owner;
    };
    struct Memory {
        Ee::CSubSystem& ee;
        void require(uint32 at,uint32 count)const {
            if(at<0x100000||count>PS2::EE_RAM_SIZE||at>PS2::EE_RAM_SIZE-count)
                extentFail(at,count);
        }
        [[noreturn]] static void extentFail(uint32 at,uint32 count) {
            std::ostringstream s;s<<"read-only EE memory extent outside RAM at 0x"<<std::hex<<at<<" size 0x"<<count;
            fail(s.str());
        }
        uint32 word(uint32 at)const {if(at&3)fail("unaligned source word address");require(at,4);uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;}
        uint32 thread()const {
            if(0x4014>PS2::EE_RAM_SIZE-4)extentFail(0x4014,4);
            uint32 value;std::memcpy(&value,ee.m_ram+0x4014,4);return value;
        }
        void bytes(std::ostringstream& out,const char* key,uint32 at,uint32 count)const {
            require(at,count);static constexpr char hex[]="0123456789abcdef";
            out<<"\""<<key<<"\":{\"address\":"<<at<<",\"bytes\":"<<count<<",\"raw_hex\":\"";
            for(uint32 i=0;i<count;++i){const uint8 v=ee.m_ram[at+i];out<<hex[v>>4]<<hex[v&15];}
            out<<"\"}";
        }
    };
    std::ofstream output_;
    std::vector<Call> calls_;
    unsigned events_=0,callsStarted_=0,rngObservations_=0;
    std::array<unsigned,9> kindCounts_{};
    std::size_t outputBytes_=0;

    [[noreturn]] static void fail(const std::string& reason) {
        throw std::runtime_error("Loader-windows observer: "+reason);
    }
    void flush(){output_.flush();if(!output_)fail("output write failed");}
    static const char* name(Kind kind) {
        switch(kind) {
        case Kind::DmaBoundary:return "178310_dma_completion_boundary";
        case Kind::VblankBoundary:return "179388_vblank_counter_boundary";
        case Kind::LoadWindow:return "178b50_loader_window";
        case Kind::LoadCallback:return "14a118_load_callback";
        case Kind::StageConstructor:return "152220_stage_constructor";
        case Kind::CameraConstructor:return "14ef78_camera_constructor";
        case Kind::PartyConstructor:return "152378_party_constructor";
        case Kind::EnemyConstructor:return "15ae60_enemy_constructor";
        case Kind::Rng:return "rng_entry_during_14a118";
        }
        return "invalid";
    }
    static bool isChildConstructor(Kind k) {
        return k==Kind::StageConstructor||k==Kind::CameraConstructor||k==Kind::PartyConstructor||k==Kind::EnemyConstructor;
    }
    const Call* activeLoaderCallback(uint32 thread)const {
        for(auto it=calls_.rbegin();it!=calls_.rend();++it)
            if(it->kind==Kind::LoadCallback&&it->thread==thread)return &*it;
        return nullptr;
    }
    void acquireDynamicBreakpoint(Ee::CSubSystem& ee,std::vector<uint32>& breakpoints,uint32 at) {
        if(std::find(breakpoints.begin(),breakpoints.end(),at)!=breakpoints.end())return;
        ee.m_EE.ToggleBreakpoint(at);breakpoints.push_back(at);
    }
    void releaseDynamicBreakpoint(Ee::CSubSystem& ee,std::vector<uint32>& breakpoints,uint32 at) {
        if(std::find(entryBreakpoints.begin(),entryBreakpoints.end(),at)!=entryBreakpoints.end())return;
        for(const auto& call:calls_)if(call.returnPc==at)return;
        const auto found=std::find(breakpoints.begin(),breakpoints.end(),at);
        if(found==breakpoints.end())return;
        ee.m_EE.ToggleBreakpoint(at);breakpoints.erase(std::remove(breakpoints.begin(),breakpoints.end(),at),breakpoints.end());
    }
    void acquireRngBreakpoint(Ee::CSubSystem& ee,std::vector<uint32>& breakpoints) {
        if(std::find(breakpoints.begin(),breakpoints.end(),rngEntry)!=breakpoints.end())return;
        ee.m_EE.ToggleBreakpoint(rngEntry);breakpoints.push_back(rngEntry);
    }
    void releaseRngBreakpointIfIdle(Ee::CSubSystem& ee,std::vector<uint32>& breakpoints) {
        if(std::any_of(calls_.begin(),calls_.end(),[](const Call& c){return c.kind==Kind::LoadCallback;}))return;
        const auto found=std::find(breakpoints.begin(),breakpoints.end(),rngEntry);
        if(found!=breakpoints.end()){ee.m_EE.ToggleBreakpoint(rngEntry);breakpoints.erase(found);}
    }
    void emit(const char* event,Kind kind,uint32 id,unsigned frame,uint32 pc,uint32 ra,uint32 sp,uint32 thread,
              uint32 entryPc,uint32 entryFrame,uint32 a0,uint32 a1,uint32 a2,uint32 a3,
              const Memory& m,uint32 owner,uint32 v0,uint32 s0,uint32 s1,uint32 rngOrdinal,
              const std::vector<const Call*>* activeCallbacks=nullptr) {
        if(++events_>maxEvents)fail("event count bound exceeded");
        std::ostringstream out;
        out<<"{\"event\":\""<<event<<"\",\"kind\":\""<<name(kind)<<"\",\"id\":"<<id
           <<",\"frame\":"<<frame<<",\"entry_frame\":"<<entryFrame<<",\"pc\":"<<pc
           <<",\"entry_pc\":"<<entryPc<<",\"ra\":"<<ra<<",\"sp\":"<<sp
           <<",\"thread_id\":"<<thread<<",\"a0\":"<<a0<<",\"a1\":"<<a1
           <<",\"a2\":"<<a2<<",\"a3\":"<<a3<<",\"v0\":"<<v0
           <<",\"s0\":"<<s0<<",\"s1\":"<<s1;
        if(rngOrdinal)out<<",\"rng_entry_ordinal\":"<<rngOrdinal;
        if(owner)out<<",\"callback_owner\":"<<owner;
        if(isChildConstructor(kind))out<<",\"paired_to_14a118\":"<<(owner?"true":"false");
        if(event[0]=='u'&&std::strncmp(event,"unpaired_",9)==0)out<<",\"paired\":false";
        if(activeCallbacks) {
            out<<",\"active_loader_callbacks\":[";
            for(std::size_t i=0;i<activeCallbacks->size();++i) {
                if(i)out<<',';
                const auto& callback=*(*activeCallbacks)[i];
                out<<"{\"id\":"<<callback.id<<",\"thread_id\":"<<callback.thread
                   <<",\"owner\":"<<callback.owner<<'}';
            }
            out<<']';
        }
        context(out,m);
        if(owner) {
            out<<",\"constructor_slots\":["<<m.word(owner+0x8064)<<','<<m.word(owner+0x8068)
               <<','<<m.word(owner+0x806c)<<','<<m.word(owner+0x8070)<<']'
               <<",\"resource_slot_8088\":"<<m.word(owner+0x8088);
        }
        out<<"}\n";
        const auto row=out.str();
        if(row.size()>262144||row.size()>maxOutputBytes-outputBytes_)fail("row/output size bound exceeded");
        outputBytes_+=row.size();output_<<row;flush();
    }
    void emitUnpairedBoundary(const char* event,Kind kind,unsigned frame,uint32 pc,uint32 ra,uint32 sp,
                              uint32 thread,uint32 a0,uint32 a1,uint32 a2,uint32 a3,uint32 v0,
                              uint32 s0,uint32 s1,const Memory& m) {
        emit(event,kind,0,frame,pc,ra,sp,thread,0,frame,a0,a1,a2,a3,m,0,v0,s0,s1,0);
    }
    void emitBoundary(const char* event,unsigned frame,uint32 pc,uint32 ra,uint32 sp,uint32 thread,
                      uint32 a0,uint32 a1,uint32 a2,uint32 a3,uint32 v0,uint32 s0,uint32 s1,const Memory& m) {
        const auto* callback=activeLoaderCallback(thread);
        if(!callback)fail("constructor continuation is not owned by 14a118 thread");
        emit(event,Kind::LoadCallback,callback->id,frame,pc,ra,sp,thread,callback->entryPc,
             callback->entryFrame,a0,a1,a2,a3,m,callback->owner,v0,s0,s1,0);
    }
    static void context(std::ostringstream& out,const Memory& m) {
        const auto mainThread=m.word(0x31fa4c),loaderThread=m.word(0x31fa50),idleThread=m.word(0x31fa54);
        const auto readIndex=m.word(0x1fe248),writeIndex=m.word(0x1fe244);
        out<<",\"thread_context\":{\"current_id\":"<<m.thread()<<",\"main_id\":"<<mainThread
           <<",\"loader_id\":"<<loaderThread<<",\"idle_id\":"<<idleThread<<",\"main_priority\":"<<m.word(0x1fe240)<<'}'
           <<",\"semaphore_handles\":{\"loader_wait\":"<<m.word(0x31fa40)
           <<",\"registration_lock\":"<<m.word(0x31fa44)<<",\"predicate_lock\":"<<m.word(0x31fa48)<<'}'
           <<",\"loader_window_state\":{\"saved_vblank\":"<<m.word(0x31fa5c)
           <<",\"active\":"<<m.word(0x31fa60)<<",\"registration_count\":"<<m.word(0x31fa64)
           <<",\"loader_refcount\":"<<m.word(0x1fe23c)<<'}'
           <<",\"queue\":{\"write_index\":"<<writeIndex<<",\"read_index\":"<<readIndex;
        if(writeIndex<32) {out<<',';m.bytes(out,"write_slot",0x31faf0+writeIndex*16,16);}
        else out<<",\"write_slot\":null";
        if(readIndex<32) {out<<',';m.bytes(out,"read_slot",0x31faf0+readIndex*16,16);}
        else out<<",\"read_slot\":null";
        out<<"},\"completion_predicate\":{\"function\":"<<m.word(0x1fe24c)
           <<",\"payload\":["<<m.word(0x31fcf0)<<','<<m.word(0x31fcf4)<<','<<m.word(0x31fcf8)<<']'
           <<'}';
        out<<',';m.bytes(out,"registered_thread_slots",0x31fa70,0x80);
        out<<',';m.bytes(out,"registered_thread_hooks",0x31fd00,0x20);
        out<<",\"display_counters\":{\"field\":"<<m.word(0x214058)
           <<",\"registration_refs\":"<<m.word(0x21405c)<<",\"vblank\":"<<m.word(0x214060)
           <<",\"saved_timer_counter\":"<<m.word(0x3b3c38)<<",\"timer_scratch\":"<<m.word(0x3b3c3c)
           <<",\"game_frame\":"<<m.word(0x24e580)<<'}';
        const auto rng=m.word(0x2151b4);
        out<<",\"rng\":{\"owner\":"<<rng;
        if(rng) {m.require(rng,0x5c);out<<",\"state\":"<<m.word(rng+0x58);}
        else out<<",\"state\":null";
        out<<'}';
    }
};
