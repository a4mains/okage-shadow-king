#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

class WanderTrace {
    struct Snapshot {
        uint32 frame=0, pc=0, a0=0, ra=0, sp=0, field=0, mode=0;
        uint32 field14=0, field64=0, field74=0, field80=0, field408=0;
        uint32 fieldCount=0, slot=0, aliasInstance=0, job=0;
        std::array<uint32,3> rosterJobs{}, rosterInstances{}, rosterInitialStates{};
        std::array<uint32,4> velocity{};
        uint32 active=0, state=0, counter=0, actor=0, cooldown=0, initial=0;
        uint32 rngPointer=0, rng=0, object=0, groupTable=0, channelBase=0, firstChannel=0, group=0, channelCount=0;
        uint32 global33958c=0, global339590=0, fcsr=0, scratchW=0;
        std::vector<std::array<uint32,4>> channels;
    } pending;
    std::ofstream output;
    unsigned pairs=0;
    bool awaitingPost=false;
public:
    static constexpr uint32 entry=0x137df8, exit=0x138020;
    static constexpr std::array<uint32,2> breakpoints={entry,exit};

    explicit WanderTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))
            throw std::runtime_error("Refusing existing wander trace");
        output.open(path);
        if(!output)throw std::runtime_error("Cannot create wander trace");
        output<<"{\"schema\":\"okage-meg-wander-pairs\",\"version\":1,\"integer_encoding\":\"unsigned raw EE words; float channels are bit patterns\",\"entry_pc\":"<<entry<<",\"exit_pc\":"<<exit<<"}\n";
        if(!output)throw std::runtime_error("Cannot write wander trace header");
    }
    void finish() const {
        if(!pairs||awaitingPost)throw std::runtime_error("Wander capture has no complete MEG callback pairs");
    }
    bool observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        auto valid=[&](uint32 at,uint32 size){return at<=PS2::EE_RAM_SIZE && size<=PS2::EE_RAM_SIZE-at;};
        auto word=[&](uint32 at){if(!valid(at,4))throw std::runtime_error("Wander trace RAM bounds");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        auto half=[&](uint32 at){return (word(at&~3u)>>((at&2)*8))&0xffff;};
        const auto pc=cpu.nPC;
        if(pc!=entry&&pc!=exit)return false;
        if(pc==entry) {
            if(awaitingPost)throw std::runtime_error("Nested MEG wander callback");
            const auto a0=uint32(cpu.nGPR[CMIPS::A0].nD0);
            const auto field=word(0x2d9bf4);
            if(!field||!valid(field,0x420))return true;
            const auto n=word(field+0x70);
            if(n!=3)return true;
            const std::array<uint32,3> expected={word(0x337b78+11*4),word(0x337b78+12*4),word(0x337b78+13*4)};
            if(expected[0]>=2048||expected[1]>=2048||expected[2]>=2048||
               expected[0]==expected[1]||expected[0]==expected[2]||expected[1]==expected[2])return true;
            std::array<uint32,3> jobs{};
            std::array<uint32,3> slots{};
            std::array<bool,3> seen{};
            for(uint32 i=0;i<n;++i) {
                const auto job=word(field+0x118+i*4);
                if(!job||!valid(job,0x5c))return true;
                const auto instance=word(job+0x40);
                unsigned match=3;
                for(unsigned k=0;k<3;++k)if(instance==expected[k])match=k;
                if(match==3||seen[match])return true;
                seen[match]=true;jobs[match]=job;slots[match]=i;
                const auto authored=word(job+0x58);
                if(authored!=(match==2?1u:0u))return true;
            }
            if(!seen[0]||!seen[1]||!seen[2])return true;
            const auto job=jobs[2];
            if(a0!=job||word(job+0x34)!=1||word(job+0x38)!=1||word(job+0x58)!=1)return true;
            const auto instance=word(job+0x40);
            const auto objects=word(0x21301c);
            if(!valid(objects,2048*0x3c))throw std::runtime_error("Wander actor table bounds");
            const auto object=objects+instance*0x3c;
            const auto first=half(object+4),group=half(object+8);
            if(group>=1024)throw std::runtime_error("Wander channel group bounds");
            const auto groupTable=word(0x213014);
            if(!valid(groupTable,1024*12))throw std::runtime_error("Wander group table bounds");
            const auto channelCount=half(groupTable+group*12+6);
            if(!channelCount||channelCount>5120||first>5120-channelCount)
                throw std::runtime_error("Wander actor channel extent");
            const auto channelBase=word(0x33760c);
            if(!valid(channelBase,0)||first+channelCount> (PS2::EE_RAM_SIZE-channelBase)/0x90)
                throw std::runtime_error("Wander actor channel RAM bounds");
            Snapshot s;
            s.frame=frame;s.pc=pc;s.a0=a0;s.ra=uint32(cpu.nGPR[CMIPS::RA].nD0);
            s.sp=uint32(cpu.nGPR[CMIPS::SP].nD0);s.field=field;s.mode=word(field+0x48);
            if(s.sp<0x54||!valid(s.sp-0x54,4))throw std::runtime_error("Wander entry scratch W bounds");
            s.fcsr=cpu.nFCSR;s.scratchW=word(s.sp-0x54);
            s.field14=word(field+0x14);s.field64=word(field+0x64);s.field74=word(field+0x74);
            s.field80=word(field+0x80);s.field408=word(field+0x408);s.fieldCount=n;s.slot=slots[2];
            s.rosterJobs=jobs;s.rosterInstances=expected;
            for(unsigned i=0;i<3;++i)s.rosterInitialStates[i]=word(jobs[i]+0x58);
            s.aliasInstance=instance;s.job=job;
            for(unsigned i=0;i<4;++i)s.velocity[i]=word(job+0x20+i*4);
            s.active=word(job+0x34);s.state=word(job+0x38);s.counter=word(job+0x3c);
            s.actor=word(job+0x40);s.cooldown=word(job+0x50);s.initial=word(job+0x58);
            s.rngPointer=word(0x2151b4);if(!valid(s.rngPointer,0x5c))throw std::runtime_error("Wander RNG pointer bounds");
            s.rng=word(s.rngPointer+0x58);s.object=object;s.groupTable=groupTable;s.channelBase=channelBase;
            s.firstChannel=first;s.group=group;s.channelCount=channelCount;
            s.global33958c=word(0x33958c);s.global339590=word(0x339590);
            s.channels.reserve(channelCount);
            for(uint32 i=0;i<channelCount;++i) {
                std::array<uint32,4> channel{};const auto at=channelBase+(first+i)*0x90+0x20;
                for(unsigned k=0;k<4;++k)channel[k]=word(at+k*4);
                s.channels.push_back(channel);
            }
            pending=std::move(s);awaitingPost=true;return true;
        }
        if(!awaitingPost)return true;
        if(++pairs>4000)throw std::runtime_error("Wander pair observation limit");
        const auto s1=uint32(cpu.nGPR[CMIPS::S1].nD0),sp=uint32(cpu.nGPR[CMIPS::SP].nD0);
        if(pending.sp<0x80||!valid(sp,0x80)||s1!=pending.job||sp!=pending.sp-0x80)
            throw std::runtime_error("Wander callback post boundary did not match saved job/stack");
        const auto savedRa=word(sp+0x70);
        if(savedRa!=pending.ra)throw std::runtime_error("Wander callback caller RA changed across pair");
        const auto field=word(0x2d9bf4);
        if(!field||!valid(field,0x420))throw std::runtime_error("Wander callback field pointer changed");
        const auto n=word(field+0x70);
        if(field!=pending.field||n!=3)throw std::runtime_error("Wander callback field/roster changed across pair");
        unsigned liveMatches=0,targetSlot=3;
        for(uint32 i=0;i<n;++i) {
            const auto job=word(field+0x118+i*4);
            if(!job||!valid(job,0x5c))throw std::runtime_error("Wander callback live job bounds");
            if(job==pending.job){++liveMatches;targetSlot=i;}
        }
        if(liveMatches!=1||targetSlot!=pending.slot||word(0x337b78+13*4)!=pending.actor||
           word(pending.job+0x40)!=pending.actor||word(pending.job+0x34)!=1||
           word(pending.job+0x58)!=1||word(pending.job+0x38)!=1)
            throw std::runtime_error("Wander callback target job changed across pair");
        const auto instance=pending.actor,objects=word(0x21301c),object=objects+instance*0x3c;
        const auto first=half(object+4),group=half(object+8),groupTable=word(0x213014);
        if(group>=1024||!valid(groupTable,1024*12))throw std::runtime_error("Wander callback channel table bounds");
        const auto channelCount=half(groupTable+group*12+6),channelBase=word(0x33760c);
        if(first!=pending.firstChannel||group!=pending.group||channelCount!=pending.channelCount||
           !valid(channelBase,0)||first+channelCount>(PS2::EE_RAM_SIZE-channelBase)/0x90)
            throw std::runtime_error("Wander callback actor channel layout changed across pair");
        Snapshot post;
        post.frame=frame;post.pc=pc;post.a0=uint32(cpu.nGPR[CMIPS::A0].nD0);post.ra=savedRa;post.sp=sp;
        post.fcsr=cpu.nFCSR;post.scratchW=word(sp+0x2c);
        post.field=field;post.mode=word(field+0x48);post.field14=word(field+0x14);
        post.field64=word(field+0x64);post.field74=word(field+0x74);post.field80=word(field+0x80);
        post.field408=word(field+0x408);post.fieldCount=n;post.slot=targetSlot;
        std::array<bool,3> postSeen{};
        for(uint32 i=0;i<n;++i) {
            const auto job=word(field+0x118+i*4);
            if(!job||!valid(job,0x5c))throw std::runtime_error("Wander post roster job bounds");
            const auto instance=word(job+0x40);
            unsigned match=3;for(unsigned k=0;k<3;++k)if(instance==word(0x337b78+(11+k)*4))match=k;
            if(match==3||postSeen[match])throw std::runtime_error("Wander post roster alias changed");
            postSeen[match]=true;
            post.rosterJobs[match]=job;post.rosterInstances[match]=instance;post.rosterInitialStates[match]=word(job+0x58);
        }
        post.aliasInstance=instance;post.job=pending.job;
        for(unsigned i=0;i<4;++i)post.velocity[i]=word(pending.job+0x20+i*4);
        post.active=word(pending.job+0x34);post.state=word(pending.job+0x38);post.counter=word(pending.job+0x3c);
        post.actor=word(pending.job+0x40);post.cooldown=word(pending.job+0x50);post.initial=word(pending.job+0x58);
        post.rngPointer=word(0x2151b4);if(!valid(post.rngPointer,0x5c))throw std::runtime_error("Wander RNG pointer bounds");
        post.rng=word(post.rngPointer+0x58);post.object=object;post.groupTable=groupTable;post.channelBase=channelBase;
        post.firstChannel=first;post.group=group;post.channelCount=channelCount;
        post.global33958c=word(0x33958c);post.global339590=word(0x339590);
        post.channels.reserve(channelCount);
        for(uint32 i=0;i<channelCount;++i) {
            std::array<uint32,4> channel{};const auto at=channelBase+(first+i)*0x90+0x20;
            for(unsigned k=0;k<4;++k)channel[k]=word(at+k*4);
            post.channels.push_back(channel);
        }
        auto emit=[&](const char* phase,const Snapshot& x) {
            output<<"{\"phase\":\""<<phase<<"\",\"frame\":"<<x.frame<<",\"pc\":"<<x.pc<<",\"a0\":"<<x.a0
                  <<",\"entry_or_saved_ra\":"<<x.ra<<",\"sp\":"<<x.sp<<",\"field\":"<<x.field
                  <<",\"field_mode_48\":"<<x.mode<<",\"field_14\":"<<x.field14<<",\"field_64\":"<<x.field64
                  <<",\"field_74\":"<<x.field74<<",\"field_80\":"<<x.field80<<",\"field_408\":"<<x.field408
                  <<",\"fcsr\":"<<x.fcsr<<",\"scratch_w_slot_u32\":"<<x.scratchW
                  <<",\"field_npc_count\":"<<x.fieldCount<<",\"npc_slot\":"<<x.slot
                  <<",\"roster_jobs\":["<<x.rosterJobs[0]<<","<<x.rosterJobs[1]<<","<<x.rosterJobs[2]<<"]"
                  <<",\"roster_instances\":["<<x.rosterInstances[0]<<","<<x.rosterInstances[1]<<","<<x.rosterInstances[2]<<"]"
                  <<",\"roster_initial_states\":["<<x.rosterInitialStates[0]<<","<<x.rosterInitialStates[1]<<","<<x.rosterInitialStates[2]<<"]"
                  <<",\"alias13_instance\":"<<x.aliasInstance<<",\"job\":"<<x.job<<",\"job_20\":[";
            for(unsigned i=0;i<4;++i)output<<(i?",":"")<<x.velocity[i];
            output<<"],\"job_34\":"<<x.active<<",\"job_38\":"<<x.state<<",\"job_3c\":"<<x.counter
                  <<",\"job_40\":"<<x.actor<<",\"job_50\":"<<x.cooldown<<",\"job_58\":"<<x.initial
                  <<",\"rng_pointer\":"<<x.rngPointer<<",\"rng_state\":"<<x.rng<<",\"actor_object\":"<<x.object
                  <<",\"group_table\":"<<x.groupTable<<",\"channel_base\":"<<x.channelBase
                  <<",\"first_channel\":"<<x.firstChannel<<",\"channel_group\":"<<x.group
                  <<",\"channel_count\":"<<x.channelCount<<",\"channels_u32\":[";
            for(std::size_t i=0;i<x.channels.size();++i)for(unsigned k=0;k<4;++k)
                output<<((i||k)?",":"")<<x.channels[i][k];
            output<<"],\"global_33958c\":"<<x.global33958c<<",\"global_339590\":"<<x.global339590<<"}";
        };
        output<<"{\"pair\":"<<pairs<<",\"entry\":";emit("pre",pending);output<<",\"exit\":";emit("post",post);output<<"}\n";
        if(!output)throw std::runtime_error("Cannot write wander trace row");
        awaitingPost=false;return true;
    }
};
