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
#include <unordered_set>
#include <vector>

class BattleLiveTrace {
public:
    static constexpr std::array<uint32,6> entryBreakpoints={0x14a248,0x14a508,0x1519f8,0x151e68,0x1524e0,0x15afa0};
    static constexpr unsigned maxFrames=4096,maxRows=65536;
    explicit BattleLiveTrace(const std::filesystem::path& path,unsigned requested):requested_(requested) {
        if(!requested||requested>maxFrames)fail("requested frame bound must be1..4096");
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))fail("refusing existing output");
        output_.open(path);if(!output_)fail("cannot create output");
        output_<<"{\"schema\":\"okage-battle-live-v1\",\"requested_vblanks\":"<<requested
          <<",\"source_elf_sha256\":\"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5\","
          <<"\"frame_convention\":\"unmodified boot frames counter:0 before first OnNewFrame; rows may span vblanks; absolute anchor is supplied by the invoking replay, never inferred\","
          <<"\"scope\":\"every observed root callback14a248/14a508/1519f8/151e68 and roster1524e0/15afa0 entry/return; no actor callback boundaries\","
          <<"\"pairing\":\"live RA and restored SP, same Play EE OS thread, root and scheduler lineage; callback slots may transition only within observed root set\","
          <<"\"raw_data\":\"pointer slots and unknown bytes remain raw; only registered entity records are emitted; model channels are existing stored records, never lazy-evaluated\","
          <<"\"thread_provenance\":\"Play PS2OS.cpp BIOS_ADDRESS_STATE_BASE4000 + BIOS_STATE.currentThreadId14; research OS identity only\","
          <<"\"limits\":{\"frames\":4096,\"rows\":65536,\"row_bytes\":524288,\"total_bytes\":1073741824}}\n";
        flush();
    }
    bool recognizes(uint32 pc) const {return isEntry(pc)||(active_&&pc==active_->returnPc);}
    void observe(Ee::CSubSystem& ee,unsigned frame,std::vector<uint32>& breakpoints) {
        const auto& cpu=ee.m_EE.m_State;Memory m{ee};
        const auto reg=[&](unsigned r){return uint32(cpu.nGPR[r].nD0);};
        const uint32 pc=uint32(cpu.nPC),sp=reg(CMIPS::SP),thread=m.thread();
        if(frame>=requested_)fail("callback beyond requested interval");
        if(lastFrame_&&frame<*lastFrame_)fail("frame counter moved backwards");lastFrame_=frame;
        if(active_&&pc==active_->returnPc) {
            const auto c=*active_;
            if(thread!=c.thread||sp!=c.sp||reg(CMIPS::RA)!=c.returnPc||m.word(0x1fce60)!=c.root)
                fail("return thread/SP/RA/root mismatch");
            validateReturn(c,m);
            snapshot(c,"return",frame,pc,reg(CMIPS::RA),sp,cpu.nFCSR,m);
            active_.reset();++pairs_;
            if(c.ownedBreakpoint){ee.m_EE.ToggleBreakpoint(c.returnPc);breakpoints.erase(std::remove(breakpoints.begin(),breakpoints.end(),c.returnPc),breakpoints.end());}
            if(c.kind==Kind::Parent&&!isParent(m.word(c.owner)))fail("root transitioned to an unobserved callback; return snapshot retained");
            return;
        }
        if(active_)fail("nested callback or wrong continuation before paired return");
        if(!isEntry(pc))fail("unrecognized breakpoint");
        const auto root=m.word(0x1fce60),owner=reg(CMIPS::A0);
        if(!root)fail("observed callback has no live root");m.require(root,0x81d0);
        if(candidateRoot_&&candidateRoot_!=root)fail("root ownership changed within bounded run");candidateRoot_=root;
        const auto kind=isParent(pc)?Kind::Parent:pc==0x1524e0?Kind::Party:Kind::Enemy;
        if(kind==Kind::Parent) {if(owner!=root)fail("parent argument differs from live root");}
        else {
            const auto offset=kind==Kind::Party?0x806cu:0x8070u;
            if(owner!=m.word(root+offset)||!reachable(root,owner,m)||m.word(owner+0x14)!=root)
                fail("roster owner is not live root-owned task");
        }
        if(m.word(owner)!=pc)fail("entry does not match live primary callback slot");
        m.require(sp,16);const auto ra=reg(CMIPS::RA);
        if((ra&3)||ra<0x100000||ra>=PS2::EE_RAM_SIZE||ra==pc)fail("invalid live return address");
        const bool owned=std::find(breakpoints.begin(),breakpoints.end(),ra)==breakpoints.end();
        Call c{kind,root,owner,sp,ra,pc,thread,frame,++calls_,links(owner,m),owned};
        snapshot(c,"entry",frame,pc,ra,sp,cpu.nFCSR,m);
        if(owned){ee.m_EE.ToggleBreakpoint(ra);breakpoints.push_back(ra);}active_=c;
    }
    void finish(unsigned completedFrames) {
        if(active_)fail("interval ended with an unpaired callback; partial trace retained");
        if(completedFrames!=requested_)fail("interval did not complete requested vblanks");
        if(!pairs_||rows_!=pairs_*2)fail("no complete live battle callback pairs");
        output_<<"{\"event\":\"finish\",\"rows\":"<<rows_<<",\"pairs\":"<<pairs_
               <<",\"completed_vblanks\":"<<completedFrames<<",\"first_callback_frame\":"<<*firstFrame_
               <<",\"last_callback_frame\":"<<*lastFrame_<<",\"root\":"<<candidateRoot_
               <<",\"callback_frame_counts\":[";
        bool comma=false;for(unsigned f=0;f<requested_;++f)if(frameRows_[f]){if(comma)output_<<',';comma=true;output_<<'['<<f<<','<<frameRows_[f]<<']';}
        output_<<"],\"coverage\":\"callback observations only; omitted frames have no observed callback, not invented state samples\"}\n";flush();
    }
private:
    enum class Kind {Parent,Party,Enemy};
    struct Links {uint32 secondary,destructor,previous,next,parent;};
    struct Call {Kind kind;uint32 root,owner,sp,returnPc,entryPc,thread,entryFrame,id;Links links;bool ownedBreakpoint;};
    struct Memory {
        Ee::CSubSystem& ee;
        void require(uint32 at,uint32 n)const {
            if((at&3)||at<0x100000||n>PS2::EE_RAM_SIZE||at>PS2::EE_RAM_SIZE-n)fail("unaligned/out-of-RAM owned extent");
        }
        uint32 word(uint32 at)const {require(at,4);uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;}
        uint16 half(uint32 at)const {if(at&1)fail("unaligned halfword");require(at&~3u,4);uint16 v;std::memcpy(&v,ee.m_ram+at,2);return v;}
        uint32 thread()const {uint32 v;std::memcpy(&v,ee.m_ram+0x4014,4);if(!v)fail("no running Play EE thread");return v;}
        void bytes(std::ostringstream& out,const char* key,uint32 at,uint32 n)const {
            // Stat binding bytes/halfwords can be unaligned to a word.
            if(at<0x100000||n>PS2::EE_RAM_SIZE||at>PS2::EE_RAM_SIZE-n)fail("raw extent outside EE RAM");
            static constexpr char hex[]="0123456789abcdef";
            out<<'"'<<key<<"\":{\"address\":"<<at<<",\"bytes\":"<<n<<",\"raw_hex\":\"";
            for(uint32 i=0;i<n;++i){auto b=ee.m_ram[at+i];out<<hex[b>>4]<<hex[b&15];}out<<"\"}";
        }
    };
    std::ofstream output_;unsigned requested_,rows_=0,calls_=0,pairs_=0;std::size_t outputBytes_=0;
    uint32 candidateRoot_=0;std::optional<unsigned> firstFrame_,lastFrame_;std::optional<Call> active_;
    std::array<unsigned,maxFrames> frameRows_{};
    [[noreturn]] static void fail(const char* text){throw std::runtime_error(std::string("Battle-live observer: ")+text);}
    void flush(){output_.flush();if(!output_)fail("output write failed");}
    static bool isParent(uint32 pc){return pc==0x14a248||pc==0x14a508||pc==0x1519f8||pc==0x151e68;}
    static bool isEntry(uint32 pc){return std::find(entryBreakpoints.begin(),entryBreakpoints.end(),pc)!=entryBreakpoints.end();}
    static Links links(uint32 p,const Memory& m){m.require(p,0x20);return {m.word(p+4),m.word(p+8),m.word(p+0xc),m.word(p+0x10),m.word(p+0x14)};}
    static bool reachable(uint32 root,uint32 target,const Memory& m) {
        struct Node{uint32 at,parent,previous;};std::vector<Node> pending;std::unordered_set<uint32> seen;bool found=false;
        if(const auto p=m.word(root+0x18))pending.push_back({p,root,0});
        while(!pending.empty()) {
            auto n=pending.back();pending.pop_back();
            if(!seen.insert(n.at).second||seen.size()>256)fail("cyclic/oversized scheduler child tree");
            m.require(n.at,0x20);
            if(m.word(n.at+0x14)!=n.parent||m.word(n.at+0xc)!=n.previous)fail("inconsistent scheduler child lineage");
            found|=n.at==target;
            if(auto p=m.word(n.at+0x10))pending.push_back({p,n.parent,n.at});
            if(auto p=m.word(n.at+0x18))pending.push_back({p,n.at,0});
        }
        return found;
    }
    static void validateReturn(const Call& c,const Memory& m) {
        const auto now=links(c.owner,m);
        if(now.secondary!=c.links.secondary||now.destructor!=c.links.destructor||now.previous!=c.links.previous||now.next!=c.links.next||now.parent!=c.links.parent)
            fail("return task identity/lineage changed");
        if(c.kind!=Kind::Parent) {
            if(m.word(c.owner)!=c.entryPc||!reachable(c.root,c.owner,m)||m.word(c.root+(c.kind==Kind::Party?0x806c:0x8070))!=c.owner)
                fail("roster ownership changed before return");
        }
    }
    static void model(std::ostringstream& out,const Memory& m,uint32 instance) {
        out<<"{\"instance_id\":"<<instance;
        if(instance==0xffffffffu){out<<",\"present\":false}";return;}
        if(instance>=640)fail("model instance outside source table");
        const auto table=m.word(0x21301c);m.require(table,640*0x3c);const auto at=table+instance*0x3c;
        out<<',';m.bytes(out,"instance",at,0x3c);
        const auto first=m.half(at+4),group=m.half(at+8),context=m.half(at+10);
        if(group>=1024)fail("model channel group outside source table");
        const auto groups=m.word(0x213014);m.require(groups,(group+1)*12);const auto g=groups+group*12;
        const auto count=m.half(g+6);if(count<3||count>5120||first>5120-count)fail("model channel extent outside source bounds");
        const auto captured=std::min<unsigned>(count,4);const auto channels=m.word(0x33760c);m.require(channels,(first+count)*0x90);
        out<<",\"first_channel\":"<<first<<",\"channel_count\":"<<count<<",\"captured_channels\":"<<captured<<',';
        m.bytes(out,"group",g,12);out<<',';m.bytes(out,"world_channels_raw",channels+first*0x90,captured*0x90);
        if(context!=0xffff){if(context>=192)fail("model context outside source table");const auto contexts=m.word(0x337614);m.require(contexts,(context+1)*24);out<<',';m.bytes(out,"script_context",contexts+context*24,24);}
        out<<'}';
    }
    void snapshot(const Call& c,const char* edge,unsigned frame,uint32 pc,uint32 ra,uint32 sp,uint32 fcsr,const Memory& m) {
        const auto count=m.word(c.root+0x50);if(count>64)fail("registered entity count exceeds64");
        std::ostringstream out;out<<"{\"event\":\"callback\",\"edge\":\""<<edge<<"\",\"kind\":\""
          <<(c.kind==Kind::Parent?"parent":c.kind==Kind::Party?"party_roster":"enemy_roster")
          <<"\",\"sequence\":"<<rows_<<",\"call_id\":"<<c.id<<",\"frame\":"<<frame
          <<",\"entry_frame\":"<<c.entryFrame<<",\"entry_pc\":"<<c.entryPc<<",\"pc\":"<<pc
          <<",\"ra\":"<<ra<<",\"sp\":"<<sp<<",\"fcsr\":"<<fcsr<<",\"thread_id\":"<<c.thread
          <<",\"root\":"<<c.root<<",\"owner\":"<<c.owner<<",\"entity_count\":"<<count<<',';
        m.bytes(out,"root_prefix",c.root,0x60);out<<',';m.bytes(out,"root_tail_queue",c.root+0x8060,0x170);
        out<<',';m.bytes(out,"registered_entities",c.root+0x60,count*0x200);
        out<<',';m.bytes(out,"owner_header",c.owner,c.kind==Kind::Parent?0x60:0x38);
        if(c.kind!=Kind::Parent) {
            const auto slots=m.word(c.owner+0x38);if(slots>(c.kind==Kind::Party?3u:64u))fail("roster slot count exceeds source bound");
            out<<",\"roster_count\":"<<slots<<',';m.bytes(out,"roster_slots",c.owner+0x38,4+4*(c.kind==Kind::Party?4:slots));
        }
        const auto entry=m.word(0x24f79c);out<<',';m.bytes(out,"battle_entry",entry,0x38);
        const auto variables=m.word(0x24f788);out<<',';m.bytes(out,"shared_variables_574",variables+0x574,4);
        const auto rng=m.word(0x2151b4);m.require(rng,0x5c);out<<",\"rng_state_word\":"<<m.word(rng+0x58)<<',';m.bytes(out,"rng_owner",rng,0x5c);
        out<<',';m.bytes(out,"pad0_latches",0x33958e,14);out<<",\"actors\":[";
        bool comma=false;
        for(unsigned i=0;i<count;++i) {
            const auto body=m.word(c.root+0x60+i*0x200+0x1f4);if(!body)continue;
            if(body<0x100040)fail("actor body before native task header");const auto task=body-0x40;
            m.require(task,0x50);const auto type=m.half(body);
            if(type>1)fail("registered actor has unrecovered body type");
            const auto roster=m.word(c.root+(type==0?0x806c:0x8070));
            if(m.word(task+0x14)!=roster||!reachable(c.root,task,m)||m.half(task+0x130)!=i)fail("registered actor ownership/entity identity mismatch");
            if(comma)out<<',';comma=true;
            out<<"{\"entity_id\":"<<i<<",\"body_pointer\":"<<body<<",\"task_pointer\":"<<task<<",\"type\":"<<type<<',';
            m.bytes(out,"task",task,type==0?0x3d0:0x3e0);out<<',';
            const auto record=m.word(task+0x50);m.bytes(out,"record",record,type==0?0x50:0x6c);
            out<<",\"stat_bindings\":[";constexpr std::array<unsigned,6> lengths={4,1,2,1,2,2};
            for(unsigned j=0;j<6;++j){if(j)out<<',';out<<'{';m.bytes(out,"value",m.word(task+0x14c+j*4),lengths[j]);out<<'}';}
            out<<"],\"model\":";model(out,m,m.word(task+0x120));out<<'}';
        }
        out<<"]}\n";const auto line=out.str();
        if(rows_>=maxRows||line.size()>512*1024||line.size()>1024*1024*1024-outputBytes_)fail("output bounds exceeded");
        if(!firstFrame_)firstFrame_=frame;++rows_;++frameRows_[frame];outputBytes_+=line.size();output_<<line;flush();
    }
};
