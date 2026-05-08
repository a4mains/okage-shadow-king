#include "okage/battle_actor.hpp"
#include "okage/battle_action_setup.hpp"
#include "okage/battle_command_action.hpp"
#include "okage/battle_command_queue.hpp"
#include "okage/battle_damage.hpp"
#include "okage/battle_enemy_commands.hpp"
#include "okage/battle_hit.hpp"
#include "okage/battle_hud.hpp"
#include "okage/battle_roster_update.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <source_location>
#include <stdexcept>
#include <string>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace {
using Callback=okage::BattleActor::FirstCallback;
void require(bool value,const char* message,const std::source_location at=std::source_location::current()) {
    if(!value)throw std::runtime_error(std::string(message)+" at line "+std::to_string(at.line()));
}
template<class F> void rejects_with(F action,const char* text) {
    try{action();}catch(const std::runtime_error& e){
        require(std::string(e.what()).find(text)!=std::string::npos,"Guard message names the unrecovered source boundary");return;}
    throw std::runtime_error(std::string("Expected rejection: ")+text);
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t value,unsigned count=4) {
    for(unsigned i=0;i<count;++i)b[at+i]=std::uint8_t(value>>(8*i));
}
std::uint32_t word(okage::Bytes b,unsigned at) {
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
int half(okage::Bytes b,unsigned at) {return std::bit_cast<std::int16_t>(std::uint16_t(b[at]|(unsigned(b[at+1])<<8)));}
void number(std::span<std::uint8_t> b,unsigned at,float value){put(b,at,std::bit_cast<std::uint32_t>(value));}
float number(okage::Bytes b,unsigned at){return std::bit_cast<float>(word(b,at));}
std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("Cannot read owned stage input "+path.string());
    return {std::istreambuf_iterator<char>(in),{}};
}
std::unique_ptr<okage::FieldActor> make_model() {
    std::vector<std::uint8_t> motion(32+20*8);
    put(motion,0,0x544d58);put(motion,4,20*8);put(motion,16,0xffffffff);put(motion,24,1);
    for(unsigned j=0;j<20;++j)put(motion,32+j*8,0x00a00002);
    okage::ModelFile model;model.models.emplace_back().bones.resize(1);
    return std::make_unique<okage::FieldActor>(std::move(model),motion,okage::TextureAnimation{},true,true);
}
//Entities: 0 Ari, 1/2 absent kind4 positions, 3 Stan, 4.. enemies. Party
//records are 0x50-byte variable rows; enemies use their constructor block.
struct Battle {
    okage::BattleState root;
    okage::BattleEntryContext entry;
    std::array<std::uint8_t,0x1000> variables{};
    std::array<okage::BattleActor,8> actors;
    std::array<std::array<std::uint8_t,0x50>,4> records{};
    std::vector<okage::BattleEntityBinding> entities;
    std::vector<std::uint32_t> rolls;unsigned draws=0;
    std::vector<okage::BattleActor*> ordinary;
    explicit Battle(unsigned enemies=1) {
        root.initialize_root({1,2,1});root.initialize_secondary();put(root.bytes,0x81a8,1);
        const unsigned count=4+enemies;put(root.bytes,0x50,count);
        for(unsigned i=0;i<count;++i) {
            auto& a=actors[i];a.entity_id=i;put(a.storage,0x130,i,2);
            const bool absent=i==1||i==2;
            entities.push_back({absent?nullptr:&a,{},absent?4u:i<4?0u:1u});
            put(root.entity(i),0x1fc,absent?0x80000000u:0u);
            root.bytes[0x60+i*0x201]=std::uint8_t(1u<<(absent?4:i<4?0:1)); //1512f0 diagonal kind bit.
            if(absent)continue;
            a.model=make_model();
            number(a.storage,0x2c,1);put(a.storage,0x40,i<4?0:1,2);put(a.storage,0x42,i<4?i:i-4,2);
            put(a.storage,0x48,i<4?0xffff:0,2);
            std::fill_n(a.storage.begin()+0x132,10,0xff);put(a.storage,0x148,0xffff,2);put(a.storage,0x14a,0xffff,2);
            put(a.storage,0x344,0xffffffffu);put(a.storage,0x12c,0xbf800000u);
            const okage::Float4 position=i<4?okage::Float4{37.5f,0,0,1}:okage::Float4{-30.f,0,float(i)*10.f-50.f,1};
            for(unsigned k=0;k<4;++k) {
                number(a.storage,0x60+4*k,position[k]);number(a.storage,0x90+4*k,okage::Float4{0,i<4?.25f:-.25f,0,0}[k]);
                number(a.storage,0xc0+4*k,1);
            }
            a.model->animation().set_channel(1,position);
            if(i<4) {
                auto& r=records[i];a.record=r;r[0]=1;put(r,8,36);put(a.storage,0x4c,0x40a00000u); //Party radius5.
                a.stat_bindings={std::span(r).subspan(8,4),std::span(r).subspan(0,1),std::span(r).subspan(0x4a,2),
                                 std::span(r).subspan(1,1),std::span(r).subspan(0x4c,2),std::span(r).subspan(0x4e,2)};
            }
        }
        put(actors[0].storage,0x46,11,2);put(actors[0].storage,0x44,0,2); //Ari LUCANB, party category11.
        put(actors[3].storage,0x46,20,2);put(actors[3].storage,0x44,6,2); //Stan source slot6.
        ordinary.push_back(&actors[0]);
    }
    okage::BattleActor& ari(){return actors[0];}
    okage::BattleActor& stan(){return actors[3];}
    //15b580's owned unit block: stats 164.., mode188/row18a, HP3c8, status3cd.
    okage::BattleActor& enemy(unsigned slot,const okage::GameDataTables& tables,int unit_id,int group) {
        auto& a=actors[4+slot];auto& c=a.storage;a.enemy=true;
        const auto& unit=tables.unit(unit_id);
        for(unsigned i=0;i<27;++i)put(c,0x350+i*4,unit[i]);
        constexpr std::array<unsigned,10> offsets={8,0x10,0x18,0x20,0x28,0x2c,0x30,0x34,0x3c,0x40};
        for(unsigned j=0;j<offsets.size();++j)put(c,0x164+j*2,std::uint16_t(half(okage::Bytes(c).subspan(0x350),offsets[j])),2);
        put(c,0x178,9,2);put(c,0x188,unit[18],2);put(c,0x18a,unit[19],2);
        put(c,0x44,std::uint32_t(unit_id),2);put(c,0x46,std::uint32_t(group),2);
        put(c,0x3c8,unit[2]);c[0x3cd]=1;
        a.stat_bindings={std::span(c).subspan(0x3c8,4),std::span(c).subspan(0x3cd,1),std::span(c).subspan(0x3ce,2),
                         std::span(c).subspan(0x3d0,1),std::span(c).subspan(0x3d2,2),{}};
        a.record=std::span(c).subspan(0x350,27*4);
        number(c,0x4c,10);put(c,0x220,0xffff,2);
        return a;
    }
    std::function<std::uint32_t()> random() {
        return [this]{require(draws<rolls.size(),"Unexpected extra RNG draw");return rolls[draws++]*32u;};
    }
    okage::BattleRosterUpdateContext roster(const okage::GameDataTables* tables=nullptr,const okage::BattleActionData* data=nullptr,
                                            const okage::BattleEnemyCommands* commands=nullptr) {
        return {root,entry,variables,entities,0,0,[](okage::BattleRosterSound){},random(),tables,data,commands};
    }
    std::vector<okage::BattleActor*> enemy_list() {
        std::vector<okage::BattleActor*> list;
        for(unsigned i=4;i<entities.size();++i)list.push_back(entities[i].actor);
        return list;
    }
};
//------------------------------------------------------------------ source shape
void state7_channel_tail() {
    Battle b;auto& a=b.ari();put(a.storage,0x21c,7);put(a.storage,0x20,5);number(a.storage,0x19c,9);
    a.model->animation().set_channel(1,{9,9,9,1});
    auto before=a.storage;
    okage::battle_actor_motion_update(a,b.entities);
    put(before,0x20,0);
    require(a.storage==before&&a.first_callback==Callback::Roster&&!a.saved_first_callback,"State7 writes only TASK20");
    for(unsigned k=0;k<4;++k)require(a.model->animation().value(1)[k]==number(a.storage,0x60+4*k),"State7 publishes channel1");
}
void state10_defend_shape() {
    for(bool enemy:{false,true}) {
        Battle b;auto& a=enemy?b.actors[4]:b.ari();
        if(enemy){a.enemy=true;a.stat_bindings[1]=std::span(a.storage).subspan(0x3cd,1);a.storage[0x3cd]=1;}
        put(a.storage,0x21c,10);put(a.storage,0x20,5);put(a.storage,0x184,0);put(a.storage,0x128,1);number(a.storage,0x12c,-1);
        a.model->animation().set_channel(1,{9,9,9,1});
        auto before=a.storage;
        if(enemy)okage::battle_enemy_first_update(a,b.root,b.entities);else okage::battle_party_first_update(a,b.root,b.entities);
        //154dc4: 1546a0(-1,14) then sw 1,BODY+144 and 99 in the delay slot.
        require(a.model->motion()==14&&word(a.storage,0x128)==14&&word(a.storage,0x12c)==0xbf800000u&&
                word(a.storage,0x184)==1&&word(a.storage,0x21c)==99&&word(a.storage,0x20)==0,
                "State10 selects motion14 for -1, sets the defending word and state99");
        for(unsigned at:{0x20u,0x128u,0x12cu,0x184u,0x21cu})put(before,at,word(a.storage,at));
        require(a.storage==before&&a.first_callback==Callback::Roster,"State10 has no other store and installs no callback");
        for(unsigned k=0;k<4;++k)require(a.model->animation().value(1)[k]==number(a.storage,0x60+4*k),"State10 keeps the 154e20 tail");
    }
    Battle b;auto& a=b.ari();put(a.storage,0x21c,10);a.model.reset();
    rejects_with([&]{okage::battle_actor_motion_update(a,b.entities);},"model owner");
    require(word(a.storage,0x128)==14&&word(a.storage,0x184)==0&&word(a.storage,0x21c)==10,
            "1546a0 stores the motion before the missing model stops it");
}
void prepare_special(Battle& b,okage::BattleActor& a,std::initializer_list<int> targets,std::int16_t action) {
    for(unsigned i=0;i<10;++i)a.storage[0x132+i]=0xff;
    unsigned n=0;for(int t:targets)a.storage[0x132+n++]=std::uint8_t(t);
    put(a.storage,0x140,n,2);put(a.storage,0x21c,9);put(a.storage,0x1fc,std::uint16_t(action),2);
    put(a.storage,0x142,std::uint16_t(action),2);
    (void)b;
}
void state9_selection() {
    struct Case {unsigned owner;int field48;Callback expected;};
    for(const auto c:{Case{0,0,Callback::SpecialApproach},Case{0,1,Callback::SpecialBallistic},Case{0,-1,Callback::SpecialBallistic},
                      Case{3,0,Callback::SpecialApproach},Case{3,1,Callback::SpecialApproach},Case{3,-1,Callback::SpecialApproach}}) {
        Battle b(2);auto& a=b.actors[c.owner];prepare_special(b,a,{4,5},35);
        put(b.actors[4].storage,0x48,std::uint32_t(c.field48),2);put(b.actors[5].storage,0x48,0,2);
        put(a.storage,0x24,19);put(a.storage,0x30,77);
        okage::battle_actor_motion_update(a,b.entities);
        require(a.first_callback==c.expected&&a.saved_first_callback==Callback::Roster&&word(a.storage,0x24)==0&&
                word(a.storage,0x30)==c.owner&&word(a.storage,0)==0&&word(a.storage,0x28)==0&&word(a.storage,0x21c)==9,
                "154d64: first target's BODY+8 zero or owner3 installs157058, otherwise156f28");
        for(unsigned k=0;k<4;++k)require(a.model->animation().value(1)[k]==number(a.storage,0x60+4*k),"Install keeps the channel tail");
    }
    {   //Only the FIRST target is consulted.
        Battle b(2);auto& a=b.ari();prepare_special(b,a,{5,4},35);put(b.actors[4].storage,0x48,1,2);
        okage::battle_actor_motion_update(a,b.entities);
        require(a.first_callback==Callback::SpecialApproach,"A ballistic second target does not change the selection");
    }
    {
        Battle b(2);auto& a=b.ari();prepare_special(b,a,{4,5},35);b.entities[5].actor=nullptr;
        put(a.storage,0x20,5);a.model->animation().set_channel(1,{9,9,9,1});std::vector<std::string> logs;
        okage::battle_actor_motion_update(a,b.entities,&b.root,[&](std::string_view t){logs.emplace_back(t);});
        require(logs==std::vector<std::string>{"OBJ:0 TGT:5 CAUTION!! ILLEGAL TARGET NUMBER!!!!\n"},"State9 logs the pinned text");
        require(a.first_callback==Callback::Roster&&word(a.storage,0x20)==0&&word(a.storage,0x21c)==9&&
                a.model->animation().value(1)==okage::Float4({9,9,9,1}),"State9 missing target returns before install and channels");
    }
    {   Battle b(2);auto& a=b.ari();prepare_special(b,a,{},35);
        rejects_with([&]{okage::battle_actor_motion_update(a,b.entities);},"state9 first target without an actor object");
        prepare_special(b,a,{4,-1},35);
        rejects_with([&]{okage::battle_actor_motion_update(a,b.entities);},"state9 target outside native entity bindings");
        require(a.first_callback==Callback::Roster,"Unsupported targets never install");
    }
}
okage::BattleActionDescriptor& secondary(okage::BattleActor& a,std::uint32_t duration_bits,std::int16_t type) {
    auto& d=a.action_secondary.emplace();put(d.bytes,0,duration_bits);put(a.storage,0x21a,std::uint16_t(type),2);
    return d;
}
okage::BattleActionDescriptor& motion_descriptor(okage::BattleActor& a,float duration) {
    auto& d=a.action_motion.emplace();number(d.bytes,0,duration);
    for(unsigned at:{0x1e4u,0x1f4u})put(a.storage,at,4);put(a.storage,0x1ec,8);number(a.storage,0x1f8,0);
    return d;
}
void special_phases() {
    {
        Battle b(1);auto& a=b.ari();prepare_special(b,a,{4},35);okage::battle_actor_motion_update(a,b.entities);
        //0x1c action keeps the raw secondary float (156c70 alone substitutes -1).
        put(a.storage,0x218,0x1c,2);secondary(a,0x435c0000u,12);motion_descriptor(a,0);
        number(a.storage,0x2c,.5f);put(a.storage,0x20,7);put(b.root.bytes,0x81a8,0);
        std::vector<std::string> events;okage::BattleStrikeServices services;
        services.action_name=[&](std::int16_t action){events.push_back("hud "+std::to_string(action));};
        okage::battle_actor_first_update(a,b.root,b.entities,services);
        require(word(a.storage,0x24)==0&&number(a.storage,0x11c)==.5f&&word(a.storage,0x20)==7&&a.model->motion()==0&&events.empty(),
                "Closed gate: no phase/TASK20 write, one 1547b8 step");
        put(b.root.bytes,0x81a8,1);const auto queue=word(b.root.bytes,0x81a4);
        okage::battle_actor_first_update(a,b.root,b.entities,services);
        require(a.model->motion()==12&&word(a.storage,0x128)==12&&word(a.storage,0x12c)==0x435c0000u,
                "Phase0 1546a0: BODY+1da motion and the raw secondary float, even for 0x1c");
        require(number(a.storage,0x11c)==.5f,"Two 1547b8 steps (.5 -> 1 resets -> .5)");
        require(word(a.storage,0x20)==0&&word(a.storage,0x24)==1&&word(a.storage,0x300)==0&&word(a.storage,0x308)==0xffffffffu&&
                events==std::vector<std::string>{"hud 35"},"Phase0 TASK20=0, HUD, BODY2c0/2c8, phase1");
        const unsigned slot=0x80a4+(queue+1)*8;
        require(word(b.root.bytes,0x81a4)==queue+1&&word(b.root.bytes,slot)==0&&word(b.root.bytes,slot+4)==1&&
                okage::battle_command_available(b.root)==0,"1517e0(owner0,1)");
        //Phase1: TASK20=1 and 1563b0's approach (motion8, 48 frames).
        number(a.storage,0x2c,1);
        okage::battle_actor_first_update(a,b.root,b.entities,services);
        require(word(a.storage,0x20)==1&&word(a.storage,0x24)==1&&a.model->motion()==8&&number(a.storage,0x12c)==48&&
                word(a.storage,0x300)==1,"Phase1 marks TASK20 and starts 1563b0");
        unsigned calls=0;
        while(word(a.storage,0x24)==1&&calls++<200)okage::battle_actor_first_update(a,b.root,b.entities,services);
        require(word(a.storage,0x24)==2&&word(a.storage,0x300)==0,"Approach completion advances to 154ea8");
        okage::battle_actor_first_update(a,b.root,b.entities,services);
        require(word(a.storage,0x24)==3&&a.model->motion()==7,"Zero-duration strike (motion7 for waza35) completes");
        calls=0;
        while(a.first_callback==Callback::SpecialApproach&&calls++<200)okage::battle_actor_first_update(a,b.root,b.entities,services);
        require(a.first_callback==Callback::Roster&&word(a.storage,0x24)==0&&word(a.storage,0x34)==0&&word(a.storage,0x21c)==99&&
                a.model->motion()==1&&a.storage[0x132]==0xff&&half(a.storage,0x140)==0&&half(a.storage,0x142)==0,
                "156670 returns to state99, clears targets and restores the saved callback");
        require(a.model->animation().value(1)==okage::Float4({37.5f,0,0,1}),"Return reaches the baseline");
        a.first_callback=Callback::SpecialApproach;put(a.storage,0x24,7);number(a.storage,0x2c,.25f);number(a.storage,0x11c,0);
        auto expected=a.storage;number(expected,0x11c,.25f);
        okage::battle_actor_first_update(a,b.root,b.entities,services);
        require(a.storage==expected&&a.first_callback==Callback::SpecialApproach,"Unlisted phases only step the model");
    }
    {
        Battle b(1);auto& a=b.ari();prepare_special(b,a,{4},35);okage::battle_actor_motion_update(a,b.entities);
        rejects_with([&]{okage::battle_actor_first_update(a,b.root,b.entities);},"157ed0 secondary descriptor owner");
        require(word(a.storage,0x128)==0&&okage::battle_command_available(b.root)==1,"No store before the secondary load");
        secondary(a,0x43200000u,12);
        rejects_with([&]{okage::battle_actor_first_update(a,b.root,b.entities);},"state9 action-name HUD168380");
        require(okage::battle_command_available(b.root)==0&&word(a.storage,0x24)==0,"The command push precedes the missing HUD owner");
    }
    {
        Battle b(1);auto& a=b.ari();prepare_special(b,a,{4},35);put(b.actors[4].storage,0x48,1,2);
        okage::battle_actor_motion_update(a,b.entities);
        require(a.first_callback==Callback::SpecialBallistic,"Nonzero target BODY+8 selects156f28 for Ari");
        secondary(a,0x3f800000u,12);motion_descriptor(a,0);
        okage::BattleStrikeServices services;services.action_name=[](std::int16_t){};
        okage::battle_actor_first_update(a,b.root,b.entities,services);
        okage::battle_actor_first_update(a,b.root,b.entities,services);
        require(word(a.storage,0x20)==1&&word(a.storage,0x24)==1&&a.model->motion()==2&&word(a.storage,0x300)==1&&
                number(a.storage,0x12c)==119,"Phase1 155658 selects ballistic motion2 (row0: 120, minus dt)");
        unsigned calls=0;
        while(a.first_callback==Callback::SpecialBallistic&&calls++<600)okage::battle_actor_first_update(a,b.root,b.entities,services);
        require(a.first_callback==Callback::Roster&&word(a.storage,0x21c)==99&&word(a.storage,0x24)==0&&a.model->motion()==1&&
                a.model->animation().value(1)==okage::Float4({37.5f,0,0,1}),"156068 returns to the baseline, state99 and the saved callback");
    }
}
//------------------------------------------------------------------ owned data
struct Owned {
    okage::GameDataTables tables;okage::BattleActionData data;okage::BattleEnemyCommands commands;okage::RuwResource ruw;
    explicit Owned(const std::filesystem::path& stage)
        :tables(read_file(stage/"CMNDATA/XDB/BKDATA.XDB"),read_file(stage/"CMNDATA/XDB/ITEM.XDB")),
         data(read_file(stage/"NATIVE_UI/battle-actions.bin")),
         commands(read_file(stage/"NATIVE_UI/battle-enemy-commands.bin")),
         ruw(read_file(stage/"NATIVE_UI/BTL.RUW")) {}
};
okage::BattleHitServices recording_hits(std::vector<std::string>& events) {
    return {[&](int id,okage::BattleActor&){events.push_back("attach "+std::to_string(id));return 4u;},
            [&](unsigned,int amount){events.push_back("number "+std::to_string(amount));},
            [&](okage::BattleActor&,std::int16_t delta,std::int16_t){events.push_back("history "+std::to_string(delta));},
            [&](okage::BattleActor&){events.push_back("death");},
            [&](int id,const okage::Float4&,bool){events.push_back("impact "+std::to_string(id));return 5u;}};
}
void rab01_defend_owned(Owned& o) {
    Battle b(1);auto& rab=b.enemy(0,o.tables,44,0);
    require(half(rab.storage,0x188)==0&&half(rab.storage,0x18a)==4&&o.commands.command(4,0)==203&&o.commands.command(4,1)==203,
            "Owned rab01 mode0 row4 starts with Defend");
    number(rab.storage,0x194,4);number(rab.storage,0x198,4);put(rab.storage,0x21c,1);put(rab.storage,0x184,0);
    auto c=b.roster(&o.tables,&o.data,&o.commands);
    const auto enemies=b.enemy_list();
    b.rolls={0,0};
    okage::battle_enemy_roster_update(enemies,c);
    require(b.draws==2&&half(rab.storage,0x18e)==0&&half(rab.storage,0x142)==203,"Two 15c3c8 draws give column0 = 203");
    require(half(rab.storage,0x144)==-1&&half(rab.storage,0x146)==4&&rab.storage[0x132]==4&&half(rab.storage,0x140)==1,
            "Waza203 type6 targets the owner itself (entity4)");
    require(rab.action_motion->identity==316&&rab.action_secondary->identity==356&&half(rab.storage,0x218)==203&&
            word(rab.storage,0x1a0)==0&&word(rab.storage,0x21c)==3,"1580d0: motion316, enemy mode1 secondary356, delay0, state3");
    put(b.root.entity(4),0x66,0x55,2);
    okage::battle_enemy_roster_update(enemies,c);
    require(word(rab.storage,0x21c)==10&&half(b.root.entity(4),0x66)==0&&b.draws==2,"1588b0 readiness1 -> 158aa8 then state10");
    rab.model->animation().set_channel(1,{1,2,3,1});
    okage::battle_enemy_first_update(rab,b.root,b.entities);
    require(word(rab.storage,0x21c)==99&&word(rab.storage,0x184)==1&&rab.model->motion()==14&&word(rab.storage,0x12c)==0xbf800000u&&
            rab.first_callback==Callback::Roster,"154b38 state10: Defend motion14, TASK184=1, state99");
    for(unsigned k=0;k<4;++k)require(rab.model->animation().value(1)[k]==number(rab.storage,0x60+4*k),"State10 channel tail");
    okage::battle_enemy_roster_update(enemies,c);
    require(word(rab.storage,0x21c)==1&&word(rab.storage,0x194)==0&&word(rab.storage,0x19c)==0&&word(rab.storage,0x184)==1&&
            half(b.root.entity(4),0x60)==-1&&b.draws==2,"15b0a8 state99: post-action keeps the defending word, no RNG");
    //154600 duration from rab01 speed TASK16c: ((100-s)^2/66+10)*5, separate toward-zero operations.
    {
        std::fenv_t saved;std::fegetenv(&saved);std::fesetround(FE_TOWARDZERO);
        volatile float s=float(half(rab.storage,0x16c)),d=100.f-s,q=d*d,r=q/66.f,a=r+10.f,t=a*5.f;
        const float expected=t;std::fesetenv(&saved);
        require(number(rab.storage,0x198)==expected,"Charge duration reset through154600");
    }
    //158e38 tail: a defending target (TASK184==1) halves after the clamp, same draws.
    const auto attack=[&](bool defending) {
        okage::BattleState root=b.root;auto& ari=b.ari();
        const auto& row=o.tables.action(200);
        for(unsigned i=0;i<13;++i)put(ari.storage,0x1ac+i*4,i==0||i==12?0:row.words[i]);
        put(ari.storage,0x1fc,200,2);put(ari.storage,0x166,40,2);put(ari.storage,0x16e,0,2);put(ari.storage,0x174,0,2);
        put(rab.storage,0x184,defending?1:0);
        std::vector<std::uint32_t> rolls{0,0,6};unsigned n=0;
        okage::battle_queue_damage_event(root,b.entities,16,0,4,[&]{require(n<rolls.size(),"Extra draw");return rolls[n++]*32u;});
        require(n==3,"Physical attack draws critical, accuracy, variance");
        auto e=root.entity(4);return half(e,0x176+unsigned(half(e,0x172))*8);
    };
    const int normal=attack(false),halved=attack(true);
    require(normal>1&&halved==normal/2,"Defend halves the resolved physical amount (toward zero)");
    put(rab.storage,0x184,1);
    //Next charge: 15b0a8 state1 clears TASK184 before 157b90.
    number(rab.storage,0x194,number(rab.storage,0x198));b.rolls={0,0,0,0};b.draws=0;
    okage::battle_enemy_roster_update(enemies,c);
    require(word(rab.storage,0x184)==0&&word(rab.storage,0x21c)==3&&b.draws==2,"Charge completion clears the defending word");
    std::cout<<"Owned rab01 Defend state10 sequence passed (normal "<<normal<<", defending "<<halved<<")\n";
}
void ari_defend_owned(Owned& o) {
    Battle b(1);b.enemy(0,o.tables,44,0);auto& ari=b.ari();
    put(ari.storage,0x21c,2);put(ari.storage,0x220,3,2);put(ari.storage,0x222,9,2);put(ari.storage,0x16c,20,2);
    auto c=b.roster(&o.tables,&o.data,&o.commands);
    okage::battle_party_roster_update(b.ordinary,b.stan(),c);
    require(half(ari.storage,0x142)==203&&ari.action_motion->identity==316&&ari.action_secondary->identity==355&&
            ari.storage[0x132]==0&&half(ari.storage,0x140)==1&&word(ari.storage,0x21c)==3&&b.draws==0,
            "Kind9 decodes Defend; 1580d0 type6 self target, party mode0 secondary355");
    okage::battle_party_roster_update(b.ordinary,b.stan(),c);
    require(word(ari.storage,0x21c)==10&&word(ari.storage,0x304)==1,"Readiness1 selects state10 for waza203");
    okage::battle_party_first_update(ari,b.root,b.entities);
    require(word(ari.storage,0x21c)==99&&word(ari.storage,0x184)==1&&ari.model->motion()==14,"154b38 state10 for the party");
    okage::battle_party_roster_update(b.ordinary,b.stan(),c);
    require(word(ari.storage,0x21c)==1&&word(ari.storage,0x184)==1&&half(b.root.entity(0),0x60)==-1,"152630 state99 post-action");
    //Queued category0 hit (amount3 from rab01) while defending.
    auto e=b.root.entity(0);put(e,0x172,1,2);put(e,0x17c,0,2);put(e,0x17e,3,2);put(e,0x180,4,2);
    std::vector<std::string> events;const auto hits=recording_hits(events);
    okage::battle_party_first_update(ari,b.root,b.entities,&hits);
    require(events==std::vector<std::string>{"attach 35","number 3","history -3","impact 38"}&&word(b.records[0],8)==33&&
            ari.model->motion()==4&&number(ari.storage,0x12c)==29&&half(ari.storage,0x148)==4&&word(ari.storage,0x21c)==1,
            "Hit reaction motion4 for 30 (decremented once) while defending");
    unsigned frames=0;
    while(ari.model->motion()==4&&frames++<40)okage::battle_party_first_update(ari,b.root,b.entities,&hits);
    require(frames==30&&ari.model->motion()==14&&word(ari.storage,0x128)==14&&number(ari.storage,0x12c)==-1,
            "153c50: reaction expiry restores the Defend motion14 while TASK184==1");
    number(ari.storage,0x194,number(ari.storage,0x198));
    okage::battle_party_roster_update(b.ordinary,b.stan(),c);
    require(word(ari.storage,0x184)==0&&ari.model->motion()==1&&word(ari.storage,0x21c)==2&&half(ari.storage,0x220)==2&&b.draws==0,
            "Full charge clears TASK184 and reselects idle motion1");
    std::cout<<"Owned Ari Defend state10 sequence passed\n";
}
//151e68 recorder (camera/voice/effect/mask boundaries in call order).
struct CommandRecorder {
    std::vector<std::string>& events;
    okage::BattleCommandActionServices services;
    explicit CommandRecorder(std::vector<std::string>& sink):events(sink) {
        services.camera_index=[&](unsigned row){events.push_back("camera-index "+std::to_string(row));};
        services.camera_anchored=[&](unsigned row,std::int32_t anchor){
            events.push_back("camera-anchored "+std::to_string(row)+" "+std::to_string(anchor));};
        services.camera_offset=[&](int offset){events.push_back("camera-offset "+std::to_string(offset));};
        services.reset_type3=[&]{events.push_back("hud-reset");};
        services.clear_roster_mask=[&](bool enemy){events.push_back(enemy?"unmask enemy":"unmask party");};
        services.strike.sound=[&](int id){events.push_back("sound "+std::to_string(id));};
        services.strike.sound_handle=[&](int id){events.push_back("voice "+std::to_string(id));return 0x51u;};
        services.strike.sound_stop=[&](std::uint32_t handle){events.push_back("voice-stop "+std::to_string(handle));};
        services.strike.positioned_effect=[&](int id,const okage::Float4&,bool){
            events.push_back("effect "+std::to_string(id));return 40u;};
    }
};
//Run 14a508's pop and 151e68 to restoration; returns the tick count.
unsigned run_command_action(Battle& b,std::vector<std::string>& events,std::int32_t expected_owner) {
    std::int32_t owner=-1;
    require(okage::battle_command_pop(b.root,b.entities,owner)==1&&owner==expected_owner,"Combat parent pops the owner's command1");
    put(b.root.bytes,0x81ac,std::uint32_t(owner));put(b.root.bytes,0x24,0);put(b.root.bytes,0x30,std::uint32_t(owner));
    CommandRecorder recorder(events);
    unsigned ticks=0;auto result=okage::BattleCommandActionResult::Running;
    while(result==okage::BattleCommandActionResult::Running&&ticks++<400)
        result=okage::battle_command_action_update(b.root,b.entities,recorder.services);
    require(result==okage::BattleCommandActionResult::Restored,"151e68 restores the combat parent");
    return ticks;
}
void stan206_owned(Owned& o) {
    Battle b(1);auto& rab=b.enemy(0,o.tables,44,0);auto& stan=b.stan();auto& ari=b.ari();okage::BattleHud hud;
    put(b.variables,0x574,1);put(ari.storage,0x3bc,7,2);put(ari.storage,0x148,4,2);put(stan.storage,0x21c,1);
    put(stan.storage,0x42,3,2);put(stan.storage,0x166,30,2);put(stan.storage,0x174,0,2);put(stan.storage,0x16e,0,2);
    auto c=b.roster(&o.tables,&o.data,&o.commands);
    okage::battle_party_roster_update(b.ordinary,stan,c);
    require(half(stan.storage,0x142)==0xce&&half(stan.storage,0x146)==4&&stan.storage[0x132]==4&&half(stan.storage,0x140)==1&&
            stan.action_motion->identity==319&&stan.action_secondary->identity==355&&half(stan.storage,0x21a)==12&&
            word(stan.storage,0x21c)==3&&half(ari.storage,0x3bc)==0&&b.draws==0,
            "152dc0: 0xce on HERO's remembered attacker, motion319, secondary355 type12");
    okage::battle_party_roster_update(b.ordinary,stan,c);
    require(word(stan.storage,0x21c)==9&&word(stan.storage,0x194)==0,"Readiness1 selects state9 for 0xce only");
    okage::battle_party_first_update(stan,b.root,b.entities);
    require(stan.first_callback==Callback::SpecialApproach&&word(stan.storage,0x30)==3,"154b38 state9 installs157058");
    std::vector<std::string> events;
    std::vector<std::uint32_t> damage_rolls{0,0,6};unsigned damage_draws=0;
    okage::BattleStrikeServices strike;
    strike.damage_event=[&](int command,int owner,int target) {
        events.push_back("damage "+std::to_string(command)+" "+std::to_string(owner)+" "+std::to_string(target));
        okage::battle_queue_damage_event(b.root,b.entities,std::int16_t(command),std::int16_t(owner),std::int16_t(target),
            [&]{require(damage_draws<damage_rolls.size(),"Extra damage draw");return damage_rolls[damage_draws++]*32u;});
    };
    strike.sound=[&](int id){events.push_back("sound "+std::to_string(id));};
    strike.positioned_effect=[&](int id,const okage::Float4&,bool){events.push_back("effect "+std::to_string(id));return 70u;};
    strike.action_name=[&](std::int16_t action){events.push_back("hud "+std::to_string(action));hud.show_action_name(o.ruw,o.tables,action);};
    okage::battle_actor_first_update(stan,b.root,b.entities,strike);
    require(stan.model->motion()==12&&number(stan.storage,0x12c)==220&&word(stan.storage,0x24)==1&&events==std::vector<std::string>{"hud 206"},
            "Phase0: secondary type12 for 220, HUD, phase1");
    const auto& text=*o.ruw.widget(5,0x2e).runtime->text;
    require(std::string(text.data())=="\\f[2]\\s[0.95,1.0]Crushing Devil"&&o.ruw.widget(5,0x2e).runtime->words[6]==std::uint32_t(320-5*14),
            "168380 shows the owned Waza name");
    events.clear();
    const unsigned ticks=run_command_action(b,events,3);
    require(ticks==221&&events==std::vector<std::string>{"camera-index 12","voice 31","effect 33","voice-stop 81",
            "camera-offset 1","hud-reset","unmask party","unmask enemy"},
            "151e68: I(0xc), secondary355 voice0x1f@-1 and owner effect33@0, expiry 221>220 stops the voice");
    require(word(stan.storage,0x304)==0xffffffffu&&okage::battle_command_available(b.root)==1,"TASK304=-1, commands reopened");
    events.clear();std::vector<unsigned> phases;unsigned frames=0;
    while(stan.first_callback==Callback::SpecialApproach&&frames++<400) {
        okage::battle_actor_first_update(stan,b.root,b.entities,strike);
        if(phases.empty()||phases.back()!=word(stan.storage,0x24))phases.push_back(word(stan.storage,0x24));
        if(word(stan.storage,0x24)==2&&stan.model->motion()!=8)require(stan.model->motion()==3,"154ea8 selects motion3 for 0xce");
    }
    require(phases==std::vector<unsigned>{1,2,3,0},"Approach, strike, return, restore");
    require(events==std::vector<std::string>{"damage 16 3 4","sound 18"},"Motion319: damage16 then sound18 at clock85");
    require(damage_draws==3,"Category0 physical: critical, accuracy, variance draws in order");
    auto e=b.root.entity(4);
    require(half(e,0x172)==1&&half(e,0x174+8)==0&&half(e,0x174+8+4)==3,"Queued category0 event from owner3 on rab01");
    require(word(stan.storage,0x21c)==99&&stan.first_callback==Callback::Roster&&stan.storage[0x132]==0xff,"156670 state99");
    okage::battle_party_roster_update(b.ordinary,stan,c);
    require(word(stan.storage,0x21c)==1&&half(b.root.entity(3),0x60)==-1&&b.draws==0,"152dc0 state99 -> state1");
    (void)rab;
    std::cout<<"Owned Stan 0xce state9/157058 sequence passed ("<<frames<<" frames)\n";
}
void ari_blaze_sword_owned(Owned& o) {
    Battle b(1);b.enemy(0,o.tables,44,0);auto& ari=b.ari();
    put(ari.storage,0x21c,2);put(ari.storage,0x220,3,2);put(ari.storage,0x222,4,2);put(ari.storage,0x224,0,2);put(ari.storage,0x226,35,2);
    put(ari.storage,0x16c,20,2);
    auto c=b.roster(&o.tables,&o.data,&o.commands);
    b.rolls={0};
    okage::battle_party_roster_update(b.ordinary,b.stan(),c);
    require(b.draws==1&&half(ari.storage,0x142)==35&&ari.storage[0x132]==4&&ari.action_motion->identity==224&&
            ari.action_secondary->identity==355&&number(ari.storage,0x1a0)==60&&word(ari.storage,0x21c)==3,
            "Kind4 decodes waza35 on rab01 with one draw; 1580d0 motion224, delay60");
    unsigned frames=0;
    while(word(ari.storage,0x21c)==3&&frames++<100) {
        okage::battle_party_first_update(ari,b.root,b.entities);
        okage::battle_party_roster_update(b.ordinary,b.stan(),c);
    }
    require(frames==60&&word(ari.storage,0x21c)==9&&half(ari.storage,0x218)==35&&half(ari.storage,0x21a)==12&&
            number(ari.storage,0x204)==-1&&number(ari.storage,0x214)==0&&b.draws==1,
            "154bbc timer reaches 60, then readiness1 -> state9 with 157ed0(35,0)");
    okage::battle_party_first_update(ari,b.root,b.entities);
    require(ari.first_callback==Callback::SpecialApproach,"rab01 child+48=0 selects157058 for Ari");
    std::vector<std::string> events;
    okage::BattleStrikeServices strike;
    strike.damage_event=[&](int command,int owner,int target){
        events.push_back("damage "+std::to_string(command)+" "+std::to_string(owner)+" "+std::to_string(target));};
    strike.sound=[&](int id){events.push_back("sound "+std::to_string(id));};
    strike.positioned_effect=[&](int id,const okage::Float4&,bool){events.push_back("effect "+std::to_string(id));return 70u;};
    strike.action_name=[&](std::int16_t action){events.push_back("hud "+std::to_string(action));};
    okage::battle_actor_first_update(ari,b.root,b.entities,strike);
    require(events==std::vector<std::string>{"hud 35"}&&ari.model->motion()==12&&number(ari.storage,0x12c)==220,"Phase0 announcement");
    events.clear();
    require(run_command_action(b,events,0)==221&&events==std::vector<std::string>{"camera-anchored 5 0","voice 31","effect 33",
            "voice-stop 81","camera-offset 1","hud-reset","unmask party","unmask enemy"},"151e68 default party row5");
    events.clear();std::vector<std::pair<unsigned,std::string>> timeline;frames=0;
    while(ari.first_callback==Callback::SpecialApproach&&frames++<400) {
        const auto before=events.size();
        okage::battle_actor_first_update(ari,b.root,b.entities,strike);
        for(auto i=before;i<events.size();++i)timeline.emplace_back(unsigned(number(ari.storage,0x1f8)),events[i]);
    }
    require(timeline==std::vector<std::pair<unsigned,std::string>>{{1,"sound 53"},{60,"sound 40"},{60,"effect 80"},
            {61,"effect 12"},{70,"damage 16 0 4"},{70,"sound 54"}},
            "154f98/1550d0/155180 against clock1f8: one entry per scheduler per tick");
    require(word(ari.storage,0x21c)==99&&ari.first_callback==Callback::Roster&&number(ari.storage,0x1f8)==100,"State99 after 156670");
    okage::battle_party_roster_update(b.ordinary,b.stan(),c);
    require(word(ari.storage,0x21c)==1&&b.draws==1,"152630 post-action, no further RNG");
    std::cout<<"Owned Ari Blaze Sword state9/157058 sequence passed\n";
}
}
int main(int argc,char** argv){try {
    state7_channel_tail();state10_defend_shape();state9_selection();special_phases();
    if(argc==2) {
        Owned owned(argv[1]);
        rab01_defend_owned(owned);ari_defend_owned(owned);stan206_owned(owned);ari_blaze_sword_owned(owned);
    } else require(argc==1,"Expected optional owned stage directory");
    std::cout<<"Battle states 7/9/10 tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
