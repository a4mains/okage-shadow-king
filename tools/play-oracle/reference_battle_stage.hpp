#pragma once
#include "ee/Ee_SubSystem.h"
#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

class BattleStageTrace {
public:
    static constexpr std::array<uint32,6> breakpoints={
        0x100c08,0x138b40,0x138d98,0x100c2c,0x101260,0x14a290
    };
    explicit BattleStageTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))
            throw std::runtime_error("Refusing existing battle-stage trace");
        output.open(path);
        if(!output)throw std::runtime_error("Cannot create battle-stage trace");
        output<<"{\"schema\":\"okage-battle-stage-v1\",\"scope\":\"one natural B_ROOM1 commit from 14a248 phase zero, before UI\","
            "\"encoding\":\"addresses and words unsigned decimal; raw_hex ascending-address bytes\","
            "\"frame_convention\":\"reference vblank counter at paused EE instruction; not a native tick\","
            "\"boundary\":\"named instruction has not executed; returns are caller continuations\","
            "\"raw_scope\":\"complete persistent allocations include pointers and untouched tails; fog upper byte is incidental; filter slot tail16 is untouched by setters\","
            "\"copy_scope\":\"STG prefix 632, cameras 224, camera-settings 252, entrances 0; remaining destination bytes preserved\","
            "\"elf_sha256\":\"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5\","
            "\"stg_sha256\":\"c8bd1824e9d596d440193cf56c1a0d297edbfd4bc47e970c3889f32af327b883\"}\n";
        flush();
    }
    void finish() {
        if(active||setting||completed!=1)
            throw std::runtime_error("Battle-stage trace requires exactly one complete commit, with no active setting");
        output<<"{\"event\":\"finish\",\"commits_completed\":"<<completed
              <<",\"settings_completed\":18,\"rows\":"<<rows<<"}\n";
        flush();
    }
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        const uint32 pc=cpu.nPC,sp=uint32(cpu.nGPR[CMIPS::SP].nD0),ra=uint32(cpu.nGPR[CMIPS::RA].nD0);
        auto reg=[&](unsigned r){return uint32(cpu.nGPR[r].nD0);};
        Memory m{ee};
        if(pc==0x100c08) {
            if(active)fail("nested commit");
            if(ra!=0x14a290)return;
            if(completed)fail("second battle commit");
            Commit c{};c.root=m.word(0x1fce60);c.resource=reg(CMIPS::A0);c.sp=sp;
            m.require(c.root,0x81d0);m.require(c.resource,0x48);
            if(m.word(c.root+0x38)!=c.resource||m.word(c.root+0x24)!=0||reg(CMIPS::S1)!=c.root)
                fail("entry root/resource/caller phase mismatch");
            c.stg=m.word(c.resource+0x44);m.require(c.stg,868);
            static constexpr std::array<uint32,8> header={0x475453,0x188,0x20,18,0x188,8,0x364,0};
            for(unsigned i=0;i<header.size();++i)if(m.word(c.stg+4*i)!=header[i])fail("STG header outside B_ROOM1 scope");
            unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(ee.m_ram+c.stg,868,digest);
            std::ostringstream hash;for(auto b:digest)hash<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b);
            if(hash.str()!="c8bd1824e9d596d440193cf56c1a0d297edbfd4bc47e970c3889f32af327b883")fail("STG preimage hash mismatch");
            std::memcpy(c.preimage.data(),ee.m_ram+c.stg,c.preimage.size());
            for(unsigned i=0;i<4;++i){c.tables[i]=m.word(0x1fb9b8+i*4);m.require(c.tables[i],i?0x348:0x278);}
            active=c;snapshot("commit_entry",frame,cpu,m,true);return;
        }
        if(!active)return;
        const auto& c=*active;
        if(m.word(0x1fce60)!=c.root||m.word(c.root+0x38)!=c.resource)fail("root/resource changed during commit");
        if(pc==0x138b40) {
            if(setting||c.applied||c.next>=18)fail("unexpected setting entry");
            // 100c08 subtracts 0x20 and 138c98 subtracts 0x60 before
            // JAL 138b40. The entry breakpoint precedes 138b40's prologue.
            if(sp!=c.sp-0x80||ra!=0x138d98)fail("setting caller/stack mismatch");
            const auto at=c.tables[0]+0x20+c.next*20;
            const auto opcode=static_cast<int32_t>(static_cast<int16_t>(m.word(at)&0xffff));
            if(reg(CMIPS::A0)!=uint32(opcode)||reg(CMIPS::A1)!=at+4||reg(CMIPS::S0)!=at+20||reg(CMIPS::S2)!=c.next+1)
                fail("setting order/operands mismatch");
            if(std::memcmp(ee.m_ram+at,c.preimage.data()+0x20+c.next*20,20))fail("copied setting differs from preimage");
            setting=Setting{at,sp};snapshot("setting_entry",frame,cpu,m,false);return;
        }
        if(pc==0x138d98) {
            if(!setting||sp!=setting->sp||reg(CMIPS::S0)!=setting->at+20||reg(CMIPS::S2)!=c.next+1)
                fail("unpaired setting return");
            snapshot("setting_return",frame,cpu,m,false);setting.reset();++active->next;return;
        }
        if(pc==0x100c2c) {
            if(setting||c.next!=18||c.applied||sp!=c.sp-0x20||reg(CMIPS::S0)!=c.resource||m.word(c.resource+0x44)!=c.stg)
                fail("apply return mismatch");
            verifyCopies(m);active->applied=true;snapshot("settings_applied_before_free",frame,cpu,m,false);return;
        }
        if(pc==0x101260) {
            if(!c.applied||c.syncEntered||setting||ra!=0x100c44||sp!=c.sp-0x20||m.word(c.resource+0x44)!=0)
                fail("effect projection sync entry mismatch");
            active->syncEntered=true;snapshot("effect_projection_sync_entry",frame,cpu,m,false);return;
        }
        if(pc==0x14a290) {
            if(!c.applied||!c.syncEntered||setting||c.next!=18||sp!=c.sp||reg(CMIPS::S1)!=c.root||
               m.word(c.resource+0x44)!=0||m.word(c.resource+0x38)!=0||m.word(c.root+0x24)!=0)
                fail("commit completion mismatch");
            verifyCopies(m);snapshot("commit_complete_before_ui",frame,cpu,m,false);
            ++completed;active.reset();return;
        }
    }
private:
    struct Memory {
        Ee::CSubSystem& ee;
        void require(uint32 at,uint32 size) const {
            if(at<0x100000||size>PS2::EE_RAM_SIZE||at>PS2::EE_RAM_SIZE-size)
                fail("observation outside EE RAM");
        }
        uint32 word(uint32 at) const {require(at,4);uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;}
        void bytes(std::ostringstream& row,const char* name,uint32 at,uint32 size) const {
            require(at,size);static constexpr char hex[]="0123456789abcdef";
            row<<",\""<<name<<"\":{\"address\":"<<at<<",\"bytes\":"<<size<<",\"raw_hex\":\"";
            for(uint32 i=0;i<size;++i){const auto b=ee.m_ram[at+i];row<<hex[b>>4]<<hex[b&15];}
            row<<"\"}";
        }
    };
    struct Commit {
        uint32 root=0,resource=0,sp=0,stg=0,next=0;
        std::array<uint32,4> tables{};
        std::array<uint8_t,868> preimage{};
        bool applied=false,syncEntered=false;
    };
    struct Setting {uint32 at,sp;};
    std::optional<Commit> active;
    std::optional<Setting> setting;
    std::ofstream output;
    unsigned completed=0,rows=0;
    [[noreturn]] static void fail(const char* why){throw std::runtime_error(std::string("Battle-stage observer: ")+why);}
    void flush(){output.flush();if(!output)fail("trace write failure");}
    void verifyCopies(const Memory& m) const {
        const auto& c=*active;
        for(unsigned i=0;i<4;++i)if(m.word(0x1fb9b8+4*i)!=c.tables[i])fail("persistent table pointer changed");
        if(m.word(0x1fb9c8)!=9)fail("camera-settings count mismatch");
        if(std::memcmp(m.ee.m_ram+c.tables[0],c.preimage.data(),0x278)||
           std::memcmp(m.ee.m_ram+c.tables[1],c.preimage.data()+392,224)||
           std::memcmp(m.ee.m_ram+c.tables[3],c.preimage.data()+616,252))fail("persistent STG copies differ");
    }
    template<class State>
    void snapshot(const char* event,unsigned frame,const State& cpu,const Memory& m,bool preimage) {
        if(++rows>64)fail("row bound exceeded");
        const auto& c=*active;std::ostringstream row;
        row<<"{\"event\":\""<<event<<"\",\"sequence\":"<<rows-1<<",\"frame\":"<<frame
           <<",\"pc\":"<<cpu.nPC<<",\"ra\":"<<uint32(cpu.nGPR[CMIPS::RA].nD0)
           <<",\"sp\":"<<uint32(cpu.nGPR[CMIPS::SP].nD0)<<",\"fcsr\":"<<cpu.nFCSR
           <<",\"root\":"<<c.root<<",\"resource\":"<<c.resource<<",\"commit_entry_sp\":"<<c.sp
           <<",\"setting_index\":"<<c.next<<",\"camera_count\":8,\"camera_setting_count\":9,\"entrance_count\":0";
        if(setting){row<<",\"setting_entry_sp\":"<<setting->sp;m.bytes(row,"setting",setting->at,20);}
        if(preimage)m.bytes(row,"stg_preimage",c.stg,868);
        m.bytes(row,"root_header",c.root,0x50);m.bytes(row,"resource_header",c.resource,0x48);
        m.bytes(row,"stg_table_globals",0x1fb9b8,20);
        static constexpr const char* names[]={"persistent_stg_prefix","persistent_cameras","persistent_entrances","persistent_camera_settings"};
        for(unsigned i=0;i<4;++i){if(m.word(0x1fb9b8+4*i)!=c.tables[i])fail("persistent table pointer changed");m.bytes(row,names[i],c.tables[i],i?0x348:0x278);}
        m.bytes(row,"projection_globals",0x1fe2b4,0x40);
        m.bytes(row,"field_projection",0x337e00,0x84);
        m.bytes(row,"current_projection_descriptor",0x332220,0x80);
        m.bytes(row,"effect_projection",0x1f8484,16);
        m.bytes(row,"light_dirty_directions_colors",0x3323a8,0x88);
        m.bytes(row,"fog",0x332470,4);m.bytes(row,"clear",0x337b74,4);
        m.bytes(row,"setting_4d_signed",0x332a60,4);
        m.bytes(row,"filter_pointer",0x332934,4);m.bytes(row,"filter_slots",m.word(0x332934),0x120);
        row<<"}\n";output<<row.str();flush();
    }
};
