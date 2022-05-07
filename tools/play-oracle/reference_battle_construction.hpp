#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

class BattleConstructionTrace {
public:
    static constexpr std::array<uint32,7> breakpoints={
        0x153618,0x153a00,0x15b580,0x15b978,0x1512f0,0x1513dc,0x1f14b0
    };
    static constexpr unsigned maxRngCalls=1'000'000;
    explicit BattleConstructionTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))
            throw std::runtime_error("Refusing existing battle-construction trace");
        output.open(path);
        if(!output)throw std::runtime_error("Cannot create battle-construction trace");
        output<<"{\"schema\":\"okage-battle-construction-v1\",\"encoding\":\"addresses and words are unsigned decimal; raw_hex is ascending-address bytes\","
                "\"frame_convention\":\"current reference vblank counter at paused EE instruction, not a native tick\","
                "\"end_boundary\":\"after last constructor child store, before register restore and return\","
                "\"rng_scope\":\"all entry calls counted; detailed rows while constructor in flight may include other scheduled work; inspect RA/SP\","
                "\"raw_scope\":\"includes pointer words and untouched allocation bytes; raw equality is not implied\","
                "\"elf_sha256\":\"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5\"}\n";
        checkOutput();
    }

    void finish() {
        if(active||registration)throw std::runtime_error("Battle-construction trace ended inside an observed call");
        if(!partyCompleted||!enemyCompleted)
            throw std::runtime_error("Battle-construction trace requires completed natural party and enemy constructors");
        output<<"{\"event\":\"finish\",\"party_completed\":"<<partyCompleted
              <<",\"enemy_completed\":"<<enemyCompleted<<",\"registrations_completed\":"<<registrationsCompleted
              <<",\"rng_entries_observed\":"<<rngCalls<<",\"rows\":"<<rows<<"}\n";
        output.flush();checkOutput();
    }

    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;const uint32 pc=cpu.nPC;
        auto reg=[&](unsigned r){return uint32(cpu.nGPR[r].nD0);};
        Memory memory{ee};
        if(pc==0x1f14b0) {
            if(++rngCalls>maxRngCalls)throw std::runtime_error("Battle-construction RNG observation limit");
            if(!active)return;
            auto row=begin("rng_entry",frame,pc,reg(CMIPS::RA),reg(CMIPS::SP));
            row<<",\"constructor_id\":"<<active->id<<",\"rng_entry_ordinal\":"<<rngCalls;
            rng(row,memory);emit(row);return;
        }
        if(pc==0x153618||pc==0x15b580) {
            if(active)throw std::runtime_error("Nested battle constructors exceed observer scope");
            if(++constructors>32)throw std::runtime_error("Battle-construction count limit");
            const bool party=pc==0x153618;
            const uint32 child=reg(CMIPS::A0),argument=reg(CMIPS::A1);
            memory.require(child,party?0x3d0:0x3e0);
            uint32 live=0;
            if(party) {
                const auto source=argument>>16;
                if(source>6)throw std::runtime_error("Battle party source-slot observer bounds");
                const auto variables=memory.word(0x24f788);memory.require(variables,0xd78);
                live=variables+0x5c4+source*0x50;
            }
            active=Constructor{constructors,child,argument,reg(CMIPS::SP),live,rngCalls,party};
            snapshot("constructor_entry",frame,pc,reg(CMIPS::RA),reg(CMIPS::SP),memory,false);
            return;
        }
        if(pc==0x153a00||pc==0x15b978) {
            if(!active||active->party!=(pc==0x153a00))
                throw std::runtime_error("Unpaired battle-construction epilogue");
            if(active->child!=reg(active->party?CMIPS::S2:CMIPS::S4)||
               reg(CMIPS::SP)+(active->party?0x50u:0x70u)!=active->sp)
                throw std::runtime_error("Battle-construction epilogue owner/stack mismatch");
            if(registration)throw std::runtime_error("Battle constructor completed inside registration");
            snapshot("constructor_complete",frame,pc,reg(CMIPS::RA),reg(CMIPS::SP),memory,true);
            if(active->party)++partyCompleted;else ++enemyCompleted;
            active.reset();return;
        }
        if(pc==0x1512f0) {
            if(registration)throw std::runtime_error("Nested battle registration exceeds observer scope");
            if(registrationsCompleted>=64)throw std::runtime_error("Battle registration count limit");
            const auto root=memory.word(0x1fce60);memory.require(root,0x81d0);
            const auto count=memory.word(root+0x50);
            if(count>=64)throw std::runtime_error("Battle registration table full");
            registration=Registration{root,count,reg(CMIPS::A0),reg(CMIPS::A1),reg(CMIPS::A2),reg(CMIPS::SP)};
            registrationSnapshot("registration_entry",frame,pc,reg(CMIPS::RA),reg(CMIPS::SP),memory);
            return;
        }
        if(pc==0x1513dc) {
            if(!registration||reg(CMIPS::SP)+0x80!=registration->sp||
               reg(CMIPS::V0)!=registration->index||memory.word(0x1fce60)!=registration->root)
                throw std::runtime_error("Unpaired battle registration epilogue");
            if(memory.word(registration->root+0x50)!=registration->index+1)
                throw std::runtime_error("Battle registration did not advance exactly one slot");
            registrationSnapshot("registration_complete",frame,pc,reg(CMIPS::RA),reg(CMIPS::SP),memory);
            ++registrationsCompleted;registration.reset();return;
        }
        throw std::runtime_error("Unexpected battle-construction observation PC");
    }

private:
    struct Memory {
        Ee::CSubSystem& ee;
        void require(uint32 at,uint32 size) const {
            if(at<0x100000||size>PS2::EE_RAM_SIZE||at>PS2::EE_RAM_SIZE-size)
                throw std::runtime_error("Battle-construction observation outside EE RAM");
        }
        uint32 word(uint32 at) const {require(at,4);uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;}
        uint32 half(uint32 at) const {require(at,2);uint16 v;std::memcpy(&v,ee.m_ram+at,2);return v;}
        void bytes(std::ostringstream& row,const char* name,uint32 at,uint32 size) const {
            require(at,size);static constexpr char digits[]="0123456789abcdef";
            row<<",\""<<name<<"\":{\"address\":"<<at<<",\"bytes\":"<<size<<",\"raw_hex\":\"";
            for(uint32 i=0;i<size;++i){auto b=ee.m_ram[at+i];row<<digits[b>>4]<<digits[b&15];}
            row<<"\"}";
        }
    };
    struct Constructor {uint32 id,child,argument,sp,live,rngStart;bool party;};
    struct Registration {uint32 root,index,body,stats,kind,sp;};
    std::ofstream output;
    std::optional<Constructor> active;
    std::optional<Registration> registration;
    unsigned constructors=0,partyCompleted=0,enemyCompleted=0,registrationsCompleted=0,rngCalls=0,rows=0;
    std::size_t outputBytes=0;

    void checkOutput() const {if(!output)throw std::runtime_error("Cannot write battle-construction trace");}
    std::ostringstream begin(const char* event,unsigned frame,uint32 pc,uint32 ra,uint32 sp) const {
        std::ostringstream row;
        row<<"{\"event\":\""<<event<<"\",\"sequence\":"<<rows<<",\"frame\":"<<frame
           <<",\"pc\":"<<pc<<",\"ra\":"<<ra<<",\"sp\":"<<sp;
        return row;
    }
    void emit(std::ostringstream& row) {
        row<<"}\n";const auto text=row.str();
        if(++rows>4096||text.size()>256*1024*1024-outputBytes)
            throw std::runtime_error("Battle-construction output limit");
        outputBytes+=text.size();output<<text;checkOutput();
    }
    static void rng(std::ostringstream& row,const Memory& memory) {
        const auto base=memory.word(0x2151b4);memory.require(base,0x5c);
        row<<",\"rng_pointer\":"<<base<<",\"rng_state\":"<<memory.word(base+0x58);
    }
    static void rootState(std::ostringstream& row,const Memory& memory) {
        const auto root=memory.word(0x1fce60);memory.require(root,0x81d0);
        const auto count=memory.word(root+0x50);
        if(count>64)throw std::runtime_error("Battle registration count outside table");
        row<<",\"battle_root\":"<<root<<",\"registered_count\":"<<count;
        memory.bytes(row,"root_header",root,0x50);
        memory.bytes(row,"registration_table",root+0x50,0x8010);
        const auto entry=memory.word(0x24f79c);memory.bytes(row,"entry_context",entry,0x38);
        rng(row,memory);
    }
    static void model(std::ostringstream& row,const Memory& memory,const char* name,uint32 instance) {
        row<<",\""<<name<<"\":{\"instance\":"<<instance;
        if(instance==0xffffffffu){row<<",\"present\":false}";return;}
        if(instance>=640)throw std::runtime_error("Battle model instance observer bounds");
        const auto objects=memory.word(0x21301c);memory.require(objects,640*0x3c);
        const auto at=objects+instance*0x3c;
        row<<",\"present\":true";memory.bytes(row,"instance_record",at,0x3c);
        const auto first=memory.half(at+4),group=memory.half(at+8),context=memory.half(at+10);
        if(group>=1024)throw std::runtime_error("Battle animation group observer bounds");
        const auto groups=memory.word(0x213014);memory.require(groups,(group+1)*12);
        const auto groupAt=groups+group*12,count=memory.half(groupAt+6);
        if(count<4||count>5120||first>5120-count)
            throw std::runtime_error("Battle animation channel observer bounds");
        const auto channels=memory.word(0x33760c);memory.require(channels,(first+count)*0x90);
        row<<",\"first_channel\":"<<first<<",\"channel_count\":"<<count<<",\"group_id\":"<<group
           <<",\"context_id\":"<<context;
        memory.bytes(row,"group",groupAt,12);
        memory.bytes(row,"channels",channels+first*0x90,count*0x90);
        if(context!=0xffff) {
            if(context>=192)throw std::runtime_error("Battle animation context observer bounds");
            const auto contexts=memory.word(0x337614);memory.require(contexts,(context+1)*24);
            const auto ctx=contexts+context*24;memory.bytes(row,"context",ctx,24);
            const auto motion=memory.word(ctx+8),pc=memory.word(ctx+20);memory.require(motion,16);
            row<<",\"motion_base\":"<<motion<<",\"motion_pc\":"<<pc
               <<",\"motion_pc_minus_base\":"<<(int64_t(pc)-motion);
        }
        row<<"}";
    }
    void snapshot(const char* event,unsigned frame,uint32 pc,uint32 ra,uint32 sp,const Memory& memory,bool complete) {
        const auto& c=*active;auto row=begin(event,frame,pc,ra,sp);
        row<<",\"constructor_id\":"<<c.id<<",\"kind\":\""<<(c.party?"party":"enemy")
           <<"\",\"argument\":"<<c.argument<<",\"child_pointer\":"<<c.child
           <<",\"body_pointer\":"<<c.child+0x40<<",\"constructor_entry_sp\":"<<c.sp
           <<",\"rng_entries_since_entry\":"<<rngCalls-c.rngStart;
        memory.bytes(row,"child",c.child,c.party?0x3d0:0x3e0);
        if(c.party) {
            row<<",\"source_slot\":"<<(c.argument>>16)<<",\"formation_ordinal\":"<<(c.argument&0xffff);
            memory.bytes(row,"live_record",c.live,0x50);
            if(complete&&memory.word(c.child+0x50)!=c.live)
                throw std::runtime_error("Battle party live-record relation mismatch");
        }
        rootState(row,memory);
        if(complete) {
            row<<",\"live_or_private_record_pointer\":"<<memory.word(c.child+0x50);
            model(row,memory,"model",memory.word(c.child+0x120));
            if(!c.party) {
                model(row,memory,"hidden_bag",memory.word(c.child+0x3bc));
                model(row,memory,"hidden_box",memory.word(c.child+0x3c0));
            }
        }
        emit(row);
    }
    void registrationSnapshot(const char* event,unsigned frame,uint32 pc,uint32 ra,uint32 sp,const Memory& memory) {
        const auto& r=*registration;auto row=begin(event,frame,pc,ra,sp);
        row<<",\"registration_index\":"<<r.index<<",\"body_pointer\":"<<r.body
           <<",\"stats_pointer\":"<<r.stats<<",\"registration_kind\":"<<r.kind
           <<",\"constructor_id\":";
        if(active)row<<active->id;else row<<"null";
        rootState(row,memory);emit(row);
    }
};
