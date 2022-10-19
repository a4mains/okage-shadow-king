#pragma once

#include "ee/Ee_SubSystem.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

class BattleCallbacksTrace {
public:
    static constexpr uint32 parentEntry=0x14a248, cameraEntry=0x14f170,
        partyPrimary=0x153a20, partySecondary=0x153ce8,
        enemyPrimary=0x15b9a0, enemySecondary=0x15c138, uiEntry=0x120ad0;
    static constexpr std::array<uint32,7> entryBreakpoints={parentEntry,cameraEntry,
        partyPrimary,partySecondary,enemyPrimary,enemySecondary,uiEntry};

    explicit BattleCallbacksTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path)) fail("refusing existing output");
        output.open(path); if(!output) fail("cannot create output");
        output << "{\"schema\":\"okage-battle-callbacks-v1\","
               << "\"scope\":\"first traversal after the source 14a248 loader poll returns with parent phase 0 to 1; camera, two party actors, three enemy actors, then first UI second-layer callback\","
               << "\"pairing\":\"live RA continuation, restored SP, same root and callback owner; no frame fitting\","
               << "\"scheduler_links\":\"header +0x0c previous sibling, +0x10 next sibling, +0x14 parent, +0x18 first child; root traversal validates reciprocal list lineage\","
               << "\"source_elf_sha256\":\"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5\","
               << "\"limits\":\"Raw owner extents include pointer words and potentially uninitialized bytes. Party live record is 0x50 bytes; enemy private record is 0x6c bytes. Only reached callback owners are captured. This is not full battle-state equivalence.\"}\n";
        flush();
    }

    bool recognizes(uint32 pc) const {
        if(active && pc==active->returnPc) return true;
        return !complete && std::find(entryBreakpoints.begin(),entryBreakpoints.end(),pc)!=entryBreakpoints.end();
    }

    void observe(Ee::CSubSystem& ee,unsigned frame,std::vector<uint32>& loaders) {
        const auto& cpu=ee.m_EE.m_State;
        const uint32 pc=uint32(cpu.nPC),sp=uint32(cpu.nGPR[CMIPS::SP].nD0);
        auto reg=[&](unsigned r){return uint32(cpu.nGPR[r].nD0);};
        Memory m{ee};
        if(active && pc==active->returnPc) {
            const auto call=*active;
            if(sp!=call.sp||reg(CMIPS::RA)!=call.returnPc||m.word(0x1fce60)!=call.root)
                fail("callback continuation did not restore SP/RA/root");
            if(call.kind==Kind::Parent) {
                const auto phase=m.word(call.root+0x24);
                if(phase==0) {
                    if(++loaderPendingReturns>1024)fail("source loader-pending parent return bound exceeded");
                    active.reset();releaseCallBreakpoint(ee,loaders,call);return;
                }
                if(phase!=1||parentDone)fail("source parent phase transition is outside phase-0 to phase-1 scope");
                validateOwnerReturn(call,m);
                emit(call.parentEntrySnapshot);
                snapshot(call,"return",frame,pc,reg(CMIPS::RA),sp,cpu.nFCSR,m,true);
                active.reset();releaseCallBreakpoint(ee,loaders,call);completed(call);return;
            }
            validateOwnerReturn(call,m);
            snapshot(call,"return",frame,pc,reg(CMIPS::RA),sp,cpu.nFCSR,m,true);
            active.reset();
            releaseCallBreakpoint(ee,loaders,call);
            completed(call);
            if(complete)releaseEntryBreakpoints(ee,loaders);
            return;
        }
        if(active) {
            if(isEntry(pc)) fail("nested target callback before its caller continuation");
            return;
        }
        if(complete) return;
        const uint32 root=m.word(0x1fce60);
        if(!root)return;
        m.require(root,0x81d0);
        Kind kind;
        uint32 owner=0;
        if(pc==parentEntry) {
            if(reg(CMIPS::A0)!=root)return;
            if(parentDone)fail("root parent callback began another traversal before required UI return");
            if(m.word(root+0x24)!=0)fail("observer attached after source phase-0 transition; traversal cannot be certified");
            if(candidateRoot&&candidateRoot!=root)fail("battle root changed during loader polling");
            candidateRoot=root;
            if(++parentAttempts>1024)fail("source parent loader-poll bound exceeded");
            kind=Kind::Parent; owner=root;
        } else {
            if(pc==cameraEntry) {kind=Kind::Camera;owner=reg(CMIPS::A0);}
            else if(pc==partyPrimary) {kind=Kind::PartyPrimary;owner=reg(CMIPS::A0);}
            else if(pc==partySecondary) {kind=Kind::PartySecondary;owner=reg(CMIPS::A0);}
            else if(pc==enemyPrimary) {kind=Kind::EnemyPrimary;owner=reg(CMIPS::A0);}
            else if(pc==enemySecondary) {kind=Kind::EnemySecondary;owner=reg(CMIPS::A0);}
            else if(pc==uiEntry) {kind=Kind::Ui;owner=reg(CMIPS::A0);}
            else return;
            if(!parentDone) {
                if(++preReadyCallbacks>100000)fail("pre-ready callback stop bound exceeded");
                return;
            }
            if(root!=candidateRoot||m.word(root+0x24)!=1)
                fail("root-owned callback arrived outside phase-1 traversal");
            if(!eligible(kind,owner,root,m))fail("root-owned callback arrived out of reviewed traversal order or owner identity");
        }
        if(rows>=40)fail("callback snapshot count exceeds fixed traversal bound");
        const uint32 returnPc=reg(CMIPS::RA);
        if((returnPc&3)||returnPc<0x100000||returnPc>=PS2::EE_RAM_SIZE||returnPc==pc)
            fail("invalid live callback return address");
        const bool installed=std::find(loaders.begin(),loaders.end(),returnPc)!=loaders.end();
        if(!installed){ee.m_EE.ToggleBreakpoint(returnPc);loaders.push_back(returnPc);}
        Call call{kind,root,owner,sp,returnPc,frame,
                  kind==Kind::Ui?OwnerLinks{}:readLinks(m,owner),{},!installed};
        active=call;
        if(kind==Kind::Parent)active->parentEntrySnapshot=parentEntrySnapshot(call,frame,pc,returnPc,sp,cpu.nFCSR,m);
        else snapshot(call,"entry",frame,pc,returnPc,sp,cpu.nFCSR,m,false);
    }

    void finish() {
        if(active || !complete || rows!=26 || !parentDone || cameraCount!=1 ||
           partyPrimaryOwners.size()!=2 || partySecondaryOwners.size()!=2 ||
           enemyPrimaryOwners.size()!=3 || enemySecondaryOwners.size()!=3 || !uiDone)
            fail("trace ended before the complete bounded callback traversal");
        sameOwners(partyPrimaryOwners,partySecondaryOwners,"party");
        sameOwners(enemyPrimaryOwners,enemySecondaryOwners,"enemy");
        output<<"{\"event\":\"finish\",\"rows\":"<<rows
              <<",\"camera_callbacks\":"<<cameraCount
              <<",\"party_primary_owners\":"<<partyPrimaryOwners.size()
              <<",\"party_secondary_owners\":"<<partySecondaryOwners.size()
              <<",\"enemy_primary_owners\":"<<enemyPrimaryOwners.size()
              <<",\"enemy_secondary_owners\":"<<enemySecondaryOwners.size()
              <<",\"ui_completed\":"<<(uiDone?"true":"false")
              <<",\"source_parent_attempts\":"<<parentAttempts
              <<",\"source_loader_pending_returns\":"<<loaderPendingReturns
              <<",\"root_owned_pre_ready_callbacks\":"<<preReadyCallbacks<<"}\n";
        flush();
    }

private:
    enum class Kind : unsigned {Parent,Camera,PartyPrimary,PartySecondary,EnemyPrimary,EnemySecondary,Ui};
    struct OwnerLinks {uint32 callback0,callback1,destructor,previous,next,parent,child,flags;};
    struct Call {Kind kind;uint32 root,owner,sp,returnPc,entryFrame;OwnerLinks links;std::string parentEntrySnapshot;bool ownsBreakpoint;};
    struct Node {uint32 pointer,parent,previous;};
    struct Memory {
        Ee::CSubSystem& ee;
        void require(uint32 at,uint32 n) const {
            if(at<0x100000||n>PS2::EE_RAM_SIZE||at>PS2::EE_RAM_SIZE-n)fail("read outside EE RAM");
        }
        uint32 word(uint32 at) const {require(at,4);uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;}
        uint16 half(uint32 at) const {require(at,2);uint16 v;std::memcpy(&v,ee.m_ram+at,2);return v;}
        void bytes(std::ostringstream& out,const char* key,uint32 at,uint32 n) const {
            require(at,n);static constexpr char hex[]="0123456789abcdef";
            out<<",\""<<key<<"\":{\"address\":"<<at<<",\"bytes\":"<<n<<",\"raw_hex\":\"";
            for(uint32 i=0;i<n;++i){const auto b=ee.m_ram[at+i];out<<hex[b>>4]<<hex[b&15];}out<<"\"}";
        }
    };
    std::ofstream output;
    std::optional<Call> active;
    std::unordered_set<uint32> partyPrimaryOwners,partySecondaryOwners,enemyPrimaryOwners,enemySecondaryOwners;
    unsigned rows=0,cameraCount=0,parentAttempts=0,loaderPendingReturns=0,preReadyCallbacks=0;
    std::size_t outputBytes=0;
    bool parentDone=false,uiDone=false,complete=false;
    uint32 candidateRoot=0;

    [[noreturn]] static void fail(const char* message){throw std::runtime_error(std::string("Battle-callback observer: ")+message);}
    void flush(){output.flush();if(!output)fail("output write failed");}
    void releaseCallBreakpoint(Ee::CSubSystem& ee,std::vector<uint32>& loaders,const Call& call) {
        if(!call.ownsBreakpoint)return;
        ee.m_EE.ToggleBreakpoint(call.returnPc);
        loaders.erase(std::remove(loaders.begin(),loaders.end(),call.returnPc),loaders.end());
    }
    void releaseEntryBreakpoints(Ee::CSubSystem& ee,std::vector<uint32>& loaders) {
        for(const auto at:entryBreakpoints) {
            auto found=std::find(loaders.begin(),loaders.end(),at);
            if(found==loaders.end())continue;
            ee.m_EE.ToggleBreakpoint(at);
            loaders.erase(std::remove(loaders.begin(),loaders.end(),at),loaders.end());
        }
    }
    static bool isEntry(uint32 pc){return std::find(entryBreakpoints.begin(),entryBreakpoints.end(),pc)!=entryBreakpoints.end();}
    static const char* name(Kind k){switch(k){case Kind::Parent:return "parent";case Kind::Camera:return "camera";case Kind::PartyPrimary:return "party_primary";case Kind::PartySecondary:return "party_secondary";case Kind::EnemyPrimary:return "enemy_primary";case Kind::EnemySecondary:return "enemy_secondary";case Kind::Ui:return "ui_second_layer";}return "invalid";}
    static OwnerLinks readLinks(const Memory& m,uint32 owner) {
        m.require(owner,0x20);
        return {m.word(owner),m.word(owner+4),m.word(owner+8),m.word(owner+0xc),
                m.word(owner+0x10),m.word(owner+0x14),m.word(owner+0x18),m.word(owner+0x1c)};
    }
    void emit(const std::string& text) {
        if(text.empty())fail("missing retained successful parent-entry snapshot");
        if(++rows>40||text.size()>512*1024||text.size()>8*1024*1024-outputBytes)
            fail("trace output bound exceeded");
        outputBytes+=text.size();output<<text;flush();
    }
    static std::string parentEntrySnapshot(const Call& c,unsigned frame,uint32 pc,uint32 ra,uint32 sp,uint32 fcsr,const Memory& m) {
        std::ostringstream row;
        row<<"{\"event\":\"parent_entry\",\"sequence\":0,\"frame\":"<<frame
           <<",\"pc\":"<<pc<<",\"ra\":"<<ra<<",\"sp\":"<<sp<<",\"fcsr\":"<<fcsr
           <<",\"root\":"<<c.root<<",\"owner\":"<<c.owner<<",\"post\":false";
        m.bytes(row,"root_header",c.root,0x81d0);
        row<<",\"root_links\":{\"previous_sibling\":"<<m.word(c.root+0xc)
           <<",\"next_sibling\":"<<m.word(c.root+0x10)<<",\"parent\":"<<m.word(c.root+0x14)
           <<",\"first_child\":"<<m.word(c.root+0x18)<<",\"flags\":"<<m.word(c.root+0x1c)
           <<",\"phase\":"<<m.word(c.root+0x24)<<"}"
           <<",\"camera_state\":null,\"camera_state_unavailable\":\"camera construction has not been proven at phase-0 entry\"}\n";
        return row.str();
    }
    static void sameOwners(const std::unordered_set<uint32>& primary,const std::unordered_set<uint32>& secondary,const char* kind){
        for(const auto owner:secondary)if(!primary.contains(owner))fail(kind);
    }

    bool reachable(uint32 root,uint32 target,const Memory& m) const {
        std::array<Node,64> stack{};unsigned count=0;std::unordered_set<uint32> seen;bool found=false;
        const auto first=m.word(root+0x18);if(first)stack[count++]={first,root,0};
        while(count) {
            const auto n=stack[--count];const auto p=n.pointer;
            if(!seen.insert(p).second)fail("cycle or duplicate node in child tree");
            if(seen.size()>64)fail("child tree exceeds 64-node cap");m.require(p,0x20);
            if(m.word(p+0x14)!=n.parent||m.word(p+0x0c)!=n.previous)
                fail("scheduler child lineage or previous-sibling link is inconsistent");
            found|=p==target;
            const auto next=m.word(p+0x10),child=m.word(p+0x18);
            if(next){if(next&3)fail("unaligned next-sibling link in child tree");if(count>=stack.size())fail("child tree exceeds 64-node cap");stack[count++]={next,n.parent,p};}
            if(child){if(child&3)fail("unaligned first-child link in child tree");if(count>=stack.size())fail("child tree exceeds 64-node cap");stack[count++]={child,p,0};}
        }
        return found;
    }
    bool eligible(Kind k,uint32 owner,uint32 root,const Memory& m) {
        if(m.word(root+0x24)!=1)return false;
        if(k==Kind::Ui){
            const auto manager=m.word(root+0x8060);
            return manager&&owner==manager&&cameraCount==1&&partyPrimaryOwners.size()==2&&
                partySecondaryOwners.size()==2&&enemyPrimaryOwners.size()==3&&enemySecondaryOwners.size()==3;
        }
        if(!owner||!reachable(root,owner,m))return false;
        if(k==Kind::Camera)return cameraCount==0&&partyPrimaryOwners.empty()&&enemyPrimaryOwners.empty()&&
            owner==m.word(root+0x8068)&&m.word(owner)==cameraEntry;
        if(k==Kind::PartyPrimary)return cameraCount==1&&enemyPrimaryOwners.empty()&&partyPrimaryOwners.size()==partySecondaryOwners.size()&&m.word(owner)==partyPrimary&&!partyPrimaryOwners.contains(owner);
        if(k==Kind::PartySecondary)return cameraCount==1&&enemyPrimaryOwners.empty()&&partyPrimaryOwners.size()==partySecondaryOwners.size()+1&&m.word(owner+4)==partySecondary&&partyPrimaryOwners.contains(owner)&&!partySecondaryOwners.contains(owner);
        if(k==Kind::EnemyPrimary)return cameraCount==1&&partyPrimaryOwners.size()==2&&partySecondaryOwners.size()==2&&enemyPrimaryOwners.size()==enemySecondaryOwners.size()&&m.word(owner)==enemyPrimary&&!enemyPrimaryOwners.contains(owner);
        if(k==Kind::EnemySecondary)return cameraCount==1&&partyPrimaryOwners.size()==2&&partySecondaryOwners.size()==2&&enemyPrimaryOwners.size()==enemySecondaryOwners.size()+1&&m.word(owner+4)==enemySecondary&&enemyPrimaryOwners.contains(owner)&&!enemySecondaryOwners.contains(owner);
        return false;
    }
    void validateOwnerReturn(const Call& c,const Memory& m) const {
        if(c.kind==Kind::Ui) {
            if(m.word(c.root+0x8060)!=c.owner)fail("UI manager owner changed during callback");
            return;
        }
        if(c.kind!=Kind::Parent&&!reachable(c.root,c.owner,m))
            fail("callback owner left the source root child tree");
        const auto now=readLinks(m,c.owner);
        if(now.previous!=c.links.previous||now.next!=c.links.next||now.parent!=c.links.parent||
           (c.kind!=Kind::Parent&&now.child!=c.links.child)||now.destructor!=c.links.destructor||now.flags!=c.links.flags)
            fail("callback owner lineage/header identity changed during callback");
        auto oneOf=[](uint32 value,std::initializer_list<uint32> allowed){
            return std::find(allowed.begin(),allowed.end(),value)!=allowed.end();
        };
        bool valid=false;
        switch(c.kind) {
        case Kind::Parent: valid=now.callback0==parentEntry&&now.callback1==c.links.callback1;break;
        case Kind::Camera: valid=now.callback0==cameraEntry&&now.callback1==c.links.callback1;break;
        case Kind::PartyPrimary: valid=oneOf(now.callback0,{partyPrimary,0x154120})&&now.callback1==partySecondary;break;
        case Kind::PartySecondary: valid=oneOf(now.callback0,{partyPrimary,0x154120,0})&&oneOf(now.callback1,{partySecondary,0x153dc0});break;
        case Kind::EnemyPrimary: valid=oneOf(now.callback0,{enemyPrimary,0x15bb90})&&now.callback1==enemySecondary;break;
        case Kind::EnemySecondary: valid=oneOf(now.callback0,{enemyPrimary,0x15bb90,0})&&oneOf(now.callback1,{enemySecondary,0x15c1a8});break;
        case Kind::Ui: break;
        }
        if(!valid)fail("callback slot changed outside reviewed source transitions");
    }
    void completed(const Call& c) {
        switch(c.kind){
        case Kind::Parent:parentDone=true;break;
        case Kind::Camera:if(++cameraCount>1)fail("more than one camera callback in first traversal");break;
        case Kind::PartyPrimary:if(!partyPrimaryOwners.insert(c.owner).second)fail("duplicate party primary owner");break;
        case Kind::PartySecondary:if(!partySecondaryOwners.insert(c.owner).second)fail("duplicate party secondary owner");break;
        case Kind::EnemyPrimary:if(!enemyPrimaryOwners.insert(c.owner).second)fail("duplicate enemy primary owner");break;
        case Kind::EnemySecondary:if(!enemySecondaryOwners.insert(c.owner).second)fail("duplicate enemy secondary owner");break;
        case Kind::Ui:uiDone=true;complete=true;break;
        }
        if(partyPrimaryOwners.size()>2||partySecondaryOwners.size()>2||enemyPrimaryOwners.size()>3||enemySecondaryOwners.size()>3)
            fail("actor callback owner count exceeds first battle roster bound");
        sameOwners(partyPrimaryOwners,partySecondaryOwners,"party secondary callback has no matching primary owner");
        sameOwners(enemyPrimaryOwners,enemySecondaryOwners,"enemy secondary callback has no matching primary owner");
    }
    void snapshot(const Call& c,const char* edge,unsigned frame,uint32 pc,uint32 ra,uint32 sp,uint32 fcsr,const Memory& m,bool post) {
        const char* event=c.kind==Kind::Parent?"parent_phase0_to1_return":nullptr;
        std::ostringstream row;row<<"{\"event\":\""<<(event?event:name(c.kind))
            <<(event?"":"_")<<(event?"":edge)<<"\",\"sequence\":"<<rows
            <<",\"frame\":"<<frame<<",\"pc\":"<<pc<<",\"ra\":"<<ra<<",\"sp\":"<<sp
            <<",\"fcsr\":"<<fcsr<<",\"root\":"<<c.root<<",\"owner\":"<<c.owner
            <<",\"post\":"<<(post?"true":"false");
        m.bytes(row,"root_header",c.root,0x81d0);
        row<<",\"root_links\":{\"previous_sibling\":"<<m.word(c.root+0xc)<<",\"next_sibling\":"<<m.word(c.root+0x10)
           <<",\"parent\":"<<m.word(c.root+0x14)<<",\"first_child\":"<<m.word(c.root+0x18)<<",\"flags\":"<<m.word(c.root+0x1c)
           <<",\"phase\":"<<m.word(c.root+0x24)<<"}";
        if(c.kind==Kind::Parent)row<<",\"parent_entry_frame\":"<<c.entryFrame
            <<",\"parent_entry_pc\":"<<parentEntry<<",\"parent_entry_sp\":"<<c.sp
            <<",\"parent_entry_ra\":"<<c.returnPc;
        if(c.kind==Kind::Parent||c.kind==Kind::Camera||c.kind==Kind::PartyPrimary||c.kind==Kind::PartySecondary||c.kind==Kind::EnemyPrimary||c.kind==Kind::EnemySecondary) {
            m.require(c.owner,0x20);
            row<<",\"owner_links\":{\"callback0\":"<<m.word(c.owner)
               <<",\"callback1\":"<<m.word(c.owner+4)<<",\"destructor\":"<<m.word(c.owner+8)
               <<",\"previous_sibling\":"<<m.word(c.owner+0xc)<<",\"next_sibling\":"<<m.word(c.owner+0x10)
               <<",\"parent\":"<<m.word(c.owner+0x14)
               <<",\"first_child\":"<<m.word(c.owner+0x18)<<",\"flags\":"<<m.word(c.owner+0x1c)<<"}";
        }
        const auto camera=m.word(c.root+0x8068);m.require(camera,0x150);row<<",\"camera_owner\":"<<camera;
        m.bytes(row,"camera_state",camera,0x150);
        const auto rng=m.word(0x2151b4);m.require(rng,0x5c);
        row<<",\"rng_pointer\":"<<rng<<",\"rng_state_word\":"<<m.word(rng+0x58);
        m.bytes(row,"rng_owner",rng,0x5c);
        if(c.kind==Kind::Camera){m.require(c.owner,0x150);m.bytes(row,"camera_callback_owner",c.owner,0x150);}
        if(c.kind==Kind::PartyPrimary||c.kind==Kind::PartySecondary){
            m.require(c.owner,0x3d0);m.bytes(row,"actor_owner",c.owner,0x3d0);
            row<<",\"source_slot\":"<<m.half(c.owner+0x44)<<",\"formation_ordinal\":"<<m.half(c.owner+0x42);
            const auto stats=m.word(c.owner+0x50);m.require(stats,0x50);row<<",\"live_stats_pointer\":"<<stats;
            m.bytes(row,"live_stats",stats,0x50);model(row,m,"model",m.word(c.owner+0x120));
        }
        if(c.kind==Kind::EnemyPrimary||c.kind==Kind::EnemySecondary){
            m.require(c.owner,0x3e0);m.bytes(row,"actor_owner",c.owner,0x3e0);
            row<<",\"enemy_ordinal\":"<<m.half(c.owner+0x42)<<",\"unit_id\":"<<m.half(c.owner+0x44);
            const auto stats=m.word(c.owner+0x50);m.require(stats,0x6c);row<<",\"private_stats_pointer\":"<<stats;
            m.bytes(row,"private_stats",stats,0x6c);model(row,m,"model",m.word(c.owner+0x120));
        }
        if(c.kind==Kind::Ui){m.require(c.owner,0xef0);m.bytes(row,"ui_manager",c.owner,0xef0);}
        row<<"}\n";emit(row.str());
    }
    static void model(std::ostringstream& row,const Memory& m,const char* key,uint32 instance) {
        row<<",\""<<key<<"\":{"<<"\"instance\":"<<instance;
        if(instance==0xffffffffu){row<<",\"present\":false}";return;}
        if(instance>=640)fail("model instance id outside 0..639");
        const auto instances=m.word(0x21301c);m.require(instances,640*0x3c);
        const auto record=instances+instance*0x3c;m.bytes(row,"instance_record",record,0x3c);
        uint16 first,group,context;std::memcpy(&first,m.ee.m_ram+record+4,2);std::memcpy(&group,m.ee.m_ram+record+8,2);std::memcpy(&context,m.ee.m_ram+record+10,2);
        if(group>=1024)fail("animation group id outside bound");
        const auto groups=m.word(0x213014);m.require(groups,(group+1)*12);const auto groupAt=groups+group*12;
        uint16 count;std::memcpy(&count,m.ee.m_ram+groupAt+6,2);
        if(count<4||count>5120||first>5120-count)fail("animation channel span outside bound");
        const auto channels=m.word(0x33760c);m.require(channels,(first+count)*0x90);
        row<<",\"first_channel\":"<<first<<",\"channel_count\":"<<count<<",\"group_id\":"<<group<<",\"context_id\":"<<context;
        m.bytes(row,"group",groupAt,12);
        m.bytes(row,"channels",channels+first*0x90,count*0x90);
        if(context!=0xffff){if(context>=192)fail("animation context id outside bound");const auto contexts=m.word(0x337614);m.require(contexts,(context+1)*24);const auto ctx=contexts+context*24;m.bytes(row,"context",ctx,24);row<<",\"motion_base\":"<<m.word(ctx+8)<<",\"motion_pc\":"<<m.word(ctx+20);}
        row<<"}";
    }
};
