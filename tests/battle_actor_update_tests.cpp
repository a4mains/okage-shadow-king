#include "okage/battle_actor.hpp"
#include "okage/battle_action_setup.hpp"
#include "okage/battle_command_queue.hpp"
#include "okage/battle_hit.hpp"
#include "okage/battle_hud.hpp"
#include "okage/battle_roster_update.hpp"
#include <bit>
#include <cfenv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <limits>
#include <source_location>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace {
void require(bool value,const char* message,const std::source_location at=std::source_location::current()) {
    if(!value)throw std::runtime_error(std::string(message)+" at line "+std::to_string(at.line()));
}
template<class F> void rejects(F action) {
    try{action();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected explicit actor callback scope rejection");
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t value,unsigned count=4) {
    for(unsigned i=0;i<count;++i)b[at+i]=std::uint8_t(value>>(8*i));
}
std::uint32_t word(okage::Bytes b,unsigned at) {
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
unsigned half(okage::Bytes b,unsigned at) {return b[at]|(unsigned(b[at+1])<<8);}
void number(std::span<std::uint8_t> b,unsigned at,float value){put(b,at,std::bit_cast<std::uint32_t>(value));}
float number(okage::Bytes b,unsigned at){return std::bit_cast<float>(word(b,at));}
struct Fixture {
    okage::BattleState root;
    std::array<std::uint8_t,0x50> record{};
    okage::BattleActor actor;
    Fixture() {
        actor.record=record;actor.stat_bindings[1]=std::span(record).subspan(0,1);
        actor.stat_bindings[3]=std::span(record).subspan(1,1);record[0]=1;
        number(actor.storage,0x2c,1);number(actor.storage,0x12c,-1);
        number(actor.storage,0x194,1);number(actor.storage,0x198,4);number(actor.storage,0x1a4,1);
        for(unsigned at:{0x60,0x90,0xc0})for(unsigned i=0;i<4;++i)number(actor.storage,at+4*i,float(at+i));
        std::vector<std::uint8_t> motion(152);
        put(motion,0,0x544d58);put(motion,4,120);put(motion,16,0xffffffff);put(motion,24,1);
        put(motion,32,48);put(motion,48,0x00a00002);put(motion,52,0);
        put(motion,96,0x00a00002); //Entry8 has its own real wait/end script.
        okage::ModelFile model;model.models.emplace_back().bones.resize(1);
        actor.model=std::make_unique<okage::FieldActor>(std::move(model),motion,okage::TextureAnimation{},true,true);
    }
    void bone_transition() {
        actor.model->animation().set_channel(4,{0,0,0,1});actor.model->animation().value(4);
        actor.model->animation().turn_component(4,0,.25f,4);
    }
};
void first_callback() {
    Fixture f;put(f.actor.storage,0x20,99);put(f.actor.storage,0x1dc,99); // Body/task decoy.
    const auto script=f.actor.model->animation().script_state();
    const auto before=f.actor.storage;okage::battle_party_first_update(f.actor,f.root);
    for(unsigned i=0;i<before.size();++i)if(i<0x20||i>=0x24)require(f.actor.storage[i]==before[i],"State0 only clears task20");
    require(word(f.actor.storage,0x20)==0,"First callback clears task20");
    for(unsigned channel=0;channel<3;++channel) {
        const auto at=channel==0?0x90:channel==1?0x60:0xc0;
        const auto value=f.actor.model->animation().value(channel);
        for(unsigned i=0;i<4;++i)require(value[i]==number(f.actor.storage,at+4*i),"All four world channel components copied");
    }
    require(f.actor.model->animation().script_state()==script,"First callback never ticks script");
    put(f.actor.storage,0x21c,1);number(f.actor.storage,0x194,3.5f);number(f.actor.storage,0x198,4);
    okage::battle_party_first_update(f.actor,f.root);require(number(f.actor.storage,0x194)==4,"State1 clamps timer to denominator");
    number(f.actor.storage,0x194,1);number(f.actor.storage,0x2c,.5f);number(f.actor.storage,0x1a4,2);
    okage::battle_party_first_update(f.actor,f.root);require(number(f.actor.storage,0x194)==2,"State1 uses actual timestep times multiplier");
    put(f.actor.storage,0x130,2,2);f.actor.entity_id=0;put(f.root.bytes,0x1d2+2*0x200,1,2);
    rejects([&]{okage::battle_party_first_update(f.actor,f.root);});
    put(f.actor.storage,0x42,3,2);put(f.actor.storage,0x130,0xffff,2);put(f.actor.storage,0x12c,0x7fc00000);
    okage::battle_party_first_update(f.actor,f.root);
    put(f.actor.storage,0x42,0,2);rejects([&]{okage::battle_party_first_update(f.actor,f.root);});
    put(f.actor.storage,0x130,0,2);number(f.actor.storage,0x12c,2);number(f.actor.storage,0x2c,.5f);
    okage::battle_party_first_update(f.actor,f.root);require(number(f.actor.storage,0x12c)==1.5f,"Unexpired motion duration decrements");
    number(f.actor.storage,0x12c,0);put(f.actor.storage,0x20,55);
    rejects([&]{okage::battle_party_first_update(f.actor,f.root);});
    require(number(f.actor.storage,0x12c)==-.5f&&word(f.actor.storage,0x20)==55,"Expiry guard preserves preceding remainder store before common callback");
    number(f.actor.storage,0x12c,-1);put(f.actor.storage,0x21c,98);
    rejects([&]{okage::battle_party_first_update(f.actor,f.root);});
    require(word(f.actor.storage,0x20)==0,"Unsupported common state rejects after source clear");
}
void charge_publication() {
    Fixture f;put(f.actor.storage,0x20,1);
    struct Case{float numerator,denominator;unsigned old,expected;};
    const Case cases[]={{1,4,0,25},{1,3,0,33},{-1,3,0,65503},{1.5f,1,0,150},
        {1.5f,1,123,123},{.5f,1,123,50},{1024,1,0,36864},{-1024,1,0,28672},
        {1.5f,1,0xffff,150},{1,1,100,100}};
    for(const auto& c:cases) {
        number(f.actor.storage,0x194,c.numerator);number(f.actor.storage,0x198,c.denominator);
        put(f.record,0x26,c.old,2);f.record[0x25]=0xa5;f.record[0x28]=0x5a;
        okage::battle_party_second_update(f.actor,f.root);
        require(half(f.record,0x26)==c.expected,"Charge truncation, no clamp, signed old value and low16 storage");
        require(f.record[0x25]==0xa5&&f.record[0x28]==0x5a,"Charge store preserves neighbors");
    }
    put(f.root.bytes,0x81b4,3);put(f.record,0x26,77,2);
    rejects([&]{okage::battle_party_second_update(f.actor,f.root);});
    require(half(f.record,0x26)==77,"Callback transition rejects before charge publication");
    put(f.root.bytes,0x81b4,0);
    for(auto bad:{0u,0x7f800000u,1u}) {
        put(f.actor.storage,0x198,bad);rejects([&]{okage::battle_party_second_update(f.actor,f.root);});
        require(half(f.record,0x26)==77,"Unsupported division leaves charge untouched");
    }
    number(f.actor.storage,0x198,1);number(f.actor.storage,0x194,30000000);
    rejects([&]{okage::battle_party_second_update(f.actor,f.root);});
    require(half(f.record,0x26)==77,"Signed32 conversion bounds guarded before narrowing");
}
void ready_actor_keeps_charge() {
    Fixture f;put(f.actor.storage,0x21c,2);put(f.actor.storage,0x20,7);
    number(f.actor.storage,0x194,3);number(f.actor.storage,0x198,4);
    number(f.actor.storage,0x19c,5);number(f.actor.storage,0x1a4,20);
    okage::battle_actor_motion_update(f.actor);
    require(word(f.actor.storage,0x20)==0&&number(f.actor.storage,0x194)==3&&
            number(f.actor.storage,0x19c)==5,"Ready actor clears task20 but neither timer advances");
    const auto position=f.actor.model->animation().value(1);
    for(unsigned i=0;i<4;++i)require(position[i]==number(f.actor.storage,0x60+4*i),
        "Ready actor still publishes its world position");
}
void action_wait_callbacks() {
    for(unsigned state:{3u,4u}) {
        Fixture f;f.bone_transition();put(f.actor.storage,0x21c,state);
        number(f.actor.storage,0x19c,2);number(f.actor.storage,0x1a0,2.5f);
        number(f.actor.storage,0x1a4,100); // Ignored by this timer.
        const auto script=f.actor.model->animation().script_state();
        okage::battle_party_first_update(f.actor,f.root);
        require(number(f.actor.storage,0x19c)==2.5f&&number(f.actor.storage,0x194)==1,
                "Action elapsed clamps; charge and multiplier are untouched");
        require(f.actor.model->animation().value(4)[0]==.0625f&&word(f.actor.storage,0x20)==0,
                "Action first callback really steps model and leaves marker0");
        okage::battle_party_second_update(f.actor,f.root);
        require(f.actor.model->animation().value(4)[0]==.125f,
                "Second callback advances again; no invented double-step suppression");
        require(f.actor.model->animation().script_state()==script&&word(f.actor.storage,0x21c)==state,
                "Waiting preserves state and does not dispatch model script");
        const auto position=f.actor.model->animation().value(1);
        for(unsigned i=0;i<4;++i)require(position[i]==number(f.actor.storage,0x60+i*4),"Action wait still publishes world channels");
    }
    Fixture fraction;fraction.bone_transition();put(fraction.actor.storage,0x21c,3);
    number(fraction.actor.storage,0x2c,.5f);number(fraction.actor.storage,0x1a0,10);
    okage::battle_party_first_update(fraction.actor,fraction.root);
    require(number(fraction.actor.storage,0x19c)==.5f&&number(fraction.actor.storage,0x11c)==.5f,
            "Fractional action timestep accumulates real model step time");
    okage::battle_party_second_update(fraction.actor,fraction.root);
    require(word(fraction.actor.storage,0x11c)==0&&fraction.actor.model->animation().value(4)[0]==.0625f,
            "Two half steps cross source model accumulator threshold");
    Fixture special;special.bone_transition();put(special.actor.storage,0x21c,99);put(special.actor.storage,0x20,99);
    const auto before=special.actor.storage;okage::battle_party_first_update(special.actor,special.root);
    for(unsigned i=0;i<before.size();++i)if(i<0x20||i>=0x24)require(special.actor.storage[i]==before[i],"State99 only clears marker before channel tail");
    require(special.actor.model->animation().value(4)[0]==0,"State99 first callback does not step");
    Fixture failure;put(failure.actor.storage,0x21c,3);number(failure.actor.storage,0x1a0,10);
    failure.actor.model.reset();rejects([&]{okage::battle_actor_motion_update(failure.actor);});
    require(number(failure.actor.storage,0x19c)==1&&word(failure.actor.storage,0x11c)==0,
            "Model failure preserves preceding elapsed and accumulator writes");
}
void attack_dispatch_and_approach() {
    using Callback=okage::BattleActor::FirstCallback;
    Fixture f,target;
    std::array<okage::BattleEntityBinding,2> entities{{{&f.actor,f.record,0},{&target.actor,target.record,1}}};
    f.actor.action_motion.emplace();put(f.actor.storage,0x21c,5);f.actor.storage[0x132]=1;
    for(unsigned i=0;i<4;++i)number(f.actor.storage,0x60+4*i,okage::Float4{0,0,0,1}[i]);
    target.actor.model->animation().set_channel(1,{0,0,60,1});
    number(f.actor.storage,0x9c,7);put(f.actor.storage,0x24,19);
    f.bone_transition();
    okage::battle_actor_first_update(f.actor,f.root,entities);
    require(f.actor.first_callback==Callback::ApproachAttack&&f.actor.saved_first_callback==Callback::Roster,
            "Reached action installs native callback and saves roster owner");
    require(word(f.actor.storage,0x24)==0&&word(f.actor.storage,0x30)==0&&word(f.actor.storage,0)==0&&word(f.actor.storage,0x28)==0,
            "Header phase/owner written; guest pointer lanes normalized");
    require(f.actor.model->motion()==0&&!f.actor.model->animation().moving(1,0)&&word(f.actor.storage,0x20)==0,
            "Selection does not invoke new callback immediately");
    okage::battle_party_second_update(f.actor,f.root);
    require(f.actor.model->animation().value(4)[0]==.0625f,"Selection frame second suffix still steps model");
    okage::battle_actor_first_update(f.actor,f.root,entities);
    require(word(f.actor.storage,0x24)==1&&word(f.actor.storage,0x300)==1&&word(f.actor.storage,0x20)==1,
            "Approach phase zero falls through once and marks model stepped");
    require(f.actor.model->motion()==8&&number(f.actor.storage,0x12c)==48&&number(f.actor.storage,0x9c)==7,
            "Authored approach motion/duration selected and rotation W preserved");
    require(number(f.actor.storage,0xf8)==48&&number(f.actor.storage,0xfc)==1&&number(f.actor.storage,0x68)==0,
            "Approach ends twelve units short and preserves baseline position");
    auto& animation=f.actor.model->animation();const auto script=animation.script_state();
    const auto moved=animation.value(1);
    require(moved[2]>0&&moved[2]<48,"First approach advances actual movement channel");
    okage::battle_party_second_update(f.actor,f.root);
    require(animation.value(1)==moved,"Second suffix does not duplicate approach model step");
    //Completion follows actual channel state. No synthetic48-frame counter.
    unsigned calls=0;
    while(word(f.actor.storage,0x24)==1&&calls++<100)okage::battle_actor_first_update(f.actor,f.root,entities);
    require(word(f.actor.storage,0x24)==2&&word(f.actor.storage,0x300)==0&&animation.value(1)[2]==48,
            "Channel completion advances callback phase and clears substate");
    require(animation.script_state()==script&&number(f.actor.storage,0x12c)==48,
            "Action handler does not execute roster prefix or model script");
    require(animation.value(0)[3]==7,"Polling orientation preserves channel W");
    okage::battle_actor_first_update(f.actor,f.root,entities);
    require(word(f.actor.storage,0x24)==3,"Zero authored strike duration completes without schedules");
    for(unsigned i=0;i<4;++i)f.actor.storage[0x13c+i]=std::uint8_t(40+i);
    okage::battle_actor_first_update(f.actor,f.root,entities);
    require(word(f.actor.storage,0x24)==3&&word(f.actor.storage,0x300)==1&&f.actor.model->motion()==8,
            "Return queues position and rotation before motion8");
    require(number(f.actor.storage,0x94)==.25f&&number(f.actor.storage,0x9c)==0&&number(f.actor.storage,0x12c)==24,
            "Return uses authored quarter-turn and duration24");
    calls=0;
    while(f.actor.first_callback==Callback::ApproachAttack&&calls++<100)okage::battle_actor_first_update(f.actor,f.root,entities);
    require(f.actor.first_callback==Callback::Roster&&word(f.actor.storage,0x24)==0&&word(f.actor.storage,0x21c)==99,
            "Actual position completion restores saved roster callback");
    require(animation.value(1)==okage::Float4({0,0,0,1})&&f.actor.model->motion()==1&&number(f.actor.storage,0x12c)==-1,
            "Return reaches baseline and selects normal motion");
    require(half(f.actor.storage,0x140)==0&&half(f.actor.storage,0x146)==0&&f.actor.storage[0x132]==255&&f.actor.storage[0x13b]==255&&f.actor.storage[0x13c]==40,
            "Return clears exactly source ten target bytes and count halfwords");
    //Shorter-than12 separation keeps the source negative travel, rather than clamping.
    f.actor.first_callback=Callback::ApproachAttack;put(f.actor.storage,0x24,0);
    f.actor.storage[0x132]=1;
    animation.set_channel(1,{0,0,0,1});target.actor.model->animation().set_channel(1,{0,0,6,1});
    okage::battle_actor_first_update(f.actor,f.root,entities);
    require(number(f.actor.storage,0xf8)==-6,"Negative travel is retained");
    //Unsupported categories stop before changing callback ownership/phase.
    f.actor.first_callback=Callback::Roster;put(f.actor.storage,0x21c,5);put(f.actor.storage,0x24,13);put(f.actor.storage,0x40,1,2);put(f.actor.storage,0x44,207,2);
    rejects([&]{okage::battle_actor_first_update(f.actor,f.root,entities);});
    require(f.actor.first_callback==Callback::Roster&&word(f.actor.storage,0x24)==13,
            "Unsupported callback choice cannot silently install approach");
}
void ranged_attack() {
    //154b38 -> 1567c8 with a nonzero descriptor word +4; 156920/156ba8 flight.
    using Callback=okage::BattleActor::FirstCallback;
    Fixture f,target;auto& a=f.actor;auto& b=a.storage;
    std::array<okage::BattleEntityBinding,2> entities{{{&a,f.record,1},{&target.actor,target.record,0}}};
    put(b,0x40,1,2);put(b,0x44,23,2);put(b,0x21c,6);b[0x132]=1;put(b,0x140,1,2);put(b,0x130,0,2);
    put(f.root.bytes,0x50,2);
    a.action_motion.emplace();number(a.action_motion->bytes,0,5);put(a.action_motion->bytes,4,0x237428);
    put(a.action_motion->bytes,0xa,3,2);put(a.action_motion->bytes,0xc,0xffff,2);
    put(b,0x1e4,0);number(b,0x1e0,0);put(b,0x1ec,8);put(b,0x1f4,4);number(b,0x1f8,0);
    for(unsigned i=0;i<4;++i)number(b,0x60+4*i,okage::Float4{0,0,0,1}[i]);
    a.model->animation().set_channel(1,{0,0,0,1});
    target.actor.model->animation().set_channel(1,{10,-4,0,1});
    okage::battle_actor_motion_update(a,entities);
    require(a.first_callback==Callback::RangedAttack&&a.saved_first_callback==Callback::Roster&&word(b,0x24)==0,
            "Descriptor word+4 selects ranged1567c8 instead of ballistic");
    okage::BattleProjectile record{1,3,{1,-2,0,0}};
    std::vector<std::string> events;okage::Float4 spawned{},rotation{};std::vector<okage::Float4> positions;
    okage::BattleStrikeServices services;
    services.projectile=[&](std::uint32_t pointer)->const okage::BattleProjectile& {
        require(pointer==0x237428,"Projectile resolves the retained descriptor word");return record;};
    services.positioned_effect=[&](int id,const okage::Float4& p,bool){require(id==11,"156920 spawns effect11");
        spawned=p;events.push_back("spawn");return 5u;};
    services.effect_rotation=[&](unsigned h,const okage::Float4& r){require(h==5,"Rotation handle");rotation=r;events.push_back("rotate");};
    services.effect_position=[&](unsigned h,const okage::Float4& p){require(h==5,"Position handle");positions.push_back(p);};
    services.damage_event=[&](int command,int owner,int victim){
        require(command==0x10&&owner==0&&victim==1,"Projectile landing queues 158ca8(0x10,owner,target)");events.push_back("damage");};
    services.effect_stop=[&](unsigned h){require(h==5,"Stop handle");events.push_back("stop");};
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(word(b,0x24)==2&&a.model->motion()==7&&events==std::vector<std::string>({"spawn","rotate"}),
            "Phase0 becomes2, strikes and launches in the same callback");
    require(spawned==okage::Float4{1,-2,0,1}&&number(b,0x2c4)==-10&&word(b,0x2f8)==1&&word(b,0x2f4)==1,
            "Launch adds record offsets; target Y uses the soft double 2.5 lift");
    require(rotation[2]==.75f&&rotation[3]==1&&positions.size()==1&&positions[0][0]>spawned[0],
            "101758 rotation W=1 and the first integration step is published");
    for(unsigned i=0;i<3;++i)okage::battle_actor_first_update(a,f.root,entities,services);
    require(events==std::vector<std::string>({"spawn","rotate","damage","stop"})&&positions.size()==4&&word(b,0x2f8)==0,
            "Flight expires after duration and retires the job once");
    unsigned calls=0;while(a.first_callback==Callback::RangedAttack&&calls++<20)okage::battle_actor_first_update(a,f.root,entities,services);
    require(a.first_callback==Callback::Roster&&word(b,0x24)==1&&word(b,0x21c)==99&&a.model->motion()==1&&number(b,0x12c)==-1,
            "Phase3 restores the saved callback with phase1 and idle motion");
    for(unsigned i=0;i<10;++i)require(b[0x132+i]==255,"1544b0 clears ten targets");
    record.duration=1;put(b,0x2f8,0);a.first_callback=Callback::RangedAttack;put(b,0x24,0);
    services.projectile=[](std::uint32_t)->const okage::BattleProjectile&{throw std::runtime_error("unstaged");};
    put(b,0x1e4,0);number(b,0x1e0,0);number(b,0x1f8,0);b[0x132]=1;put(b,0x140,1,2);
    rejects([&]{okage::battle_actor_first_update(a,f.root,entities,services);});
}
void ballistic_attack() {
    using Callback=okage::BattleActor::FirstCallback;
    Fixture f,target;auto& a=f.actor;auto& b=a.storage;
    std::array<okage::BattleEntityBinding,2> entities{{{&a,f.record,1},{&target.actor,target.record,0}}};
    put(b,0x40,1,2);put(b,0x44,23,2);put(b,0x21c,6);b[0x132]=1;
    a.action_motion.emplace();number(a.action_motion->bytes,0,0);
    for(unsigned i=0;i<4;++i) {
        number(b,0x60+4*i,okage::Float4{0,0,0,1}[i]);
        number(b,0x90+4*i,okage::Float4{0,.25f,0,7}[i]);
    }
    number(b,0x4c,10);number(b,0x8c,0);
    target.actor.model->animation().set_channel(1,{30,4,0,1});
    put(f.root.entity(0),0x66,0,2);
    okage::battle_actor_motion_update(a,entities);
    require(a.first_callback==Callback::BallisticAttack&&a.saved_first_callback==Callback::Roster&&word(b,0x24)==0,
            "Enemy state6 selects155558 before executing its phases");
    okage::BattleStrikeServices services;
    unsigned scratch_reads=0;
    services.approach_offset_w=[&](const okage::BattleActor& owner) {require(&owner==&a,"Scratch owner explicit");++scratch_reads;return .5f;};
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(word(b,0x24)==1&&word(b,0x300)==1&&a.model->motion()==2&&number(b,0x12c)==119,
            "155558 phase0 falls through; approach selects motion2 and subtracts timestep immediately");
    const auto approach_script=a.model->animation().script_state();
    for(unsigned i=0;i<23;++i)okage::battle_actor_first_update(a,f.root,entities,services);
    require(a.model->animation().script_state()==approach_script,"Waiting callback steps channels, not model scripts");
    require(number(b,0x12c)==96&&scratch_reads==0&&word(b,0x300)==1,"Approach preparation waits while remaining exceeds96");
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(word(b,0x300)==2&&scratch_reads==1&&number(b,0x12c)==95&&number(b,0xfc)==1.5f,
            "Threshold equality prepares target with explicitly supplied W and then decrements");
    require(number(b,0x104)==-.125f&&a.model->animation().moving(0,0),
            "Group0 yaw-.25 plus target1 offset.125 queues76/timestep turn");
    const auto velocity=number(b,0x70),oldx=a.model->animation().value(1)[0];
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(a.model->animation().value(1)[0]==oldx+velocity&&number(b,0x12c)==94,
            "Ballistic motion adds stored velocity rather than interpolation");
    unsigned calls=0;
    while(word(b,0x24)==1&&calls++<150)okage::battle_actor_first_update(a,f.root,entities,services);
    require(word(b,0x24)==2&&number(b,0x12c)==-1&&word(b,0x300)==0&&number(b,0xf4)==0,
            "Ground clamp then countdown completion advance only on following callbacks");
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(word(b,0x24)==3,"Ballistic callback delegates actual zero-duration strike scheduler");
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(word(b,0x300)==1&&number(b,0x12c)==40&&a.model->motion()==2,"Return initialization does not decrement duration");
    for(unsigned i=0;i<39;++i)okage::battle_actor_first_update(a,f.root,entities,services);
    require(a.first_callback==Callback::BallisticAttack&&number(b,0x12c)==1,"Return remains active before equality");
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(a.first_callback==Callback::Roster&&word(b,0x24)==0&&word(b,0x21c)==99&&word(b,0x300)==0,
            "Return equality restores saved callback and resets exact action state");
    require(a.model->animation().value(1)==okage::Float4({0,0,0,1})&&a.model->motion()==1&&number(b,0x12c)==-1,
            "Return publishes original full baseline position and idle motion");
    require(scratch_reads==1,"Approach offset scratch is read only at actual launch");
    for(unsigned i=0;i<10;++i)require(b[0x132+i]==255,"Return clears ten targets");

    //Unavailable source W selects native XYZ motion; it never supplies a scratch value.
    a.first_callback=Callback::BallisticAttack;put(b,0x24,1);put(b,0x300,1);number(b,0x12c,96);b[0x132]=1;
    services.approach_offset_w={};
    number(b,0x7c,13);
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(a.cartesian_motion&&word(b,0x300)==2&&number(b,0x12c)==95&&number(b,0x7c)==13,
            "Cartesian adapter launches without evaluating unavailable velocity W");
    put(b,0xfc,0x7fc12345);
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(a.model->animation().value(1)[3]==1&&number(b,0x7c)==13&&word(b,0xfc)==0x7fc12345,
            "Native model point is homogeneous while scratch W remains uninterpreted");
    a.cartesian_motion=false;put(b,0x300,1);number(b,0x12c,96);
    services.approach_offset_w=[](const okage::BattleActor&){return 0.f;};
    //Diagonal bit0 chooses live target radius and facing, ignoring group angle.
    f.root.bytes[0x60]=1;put(f.root.entity(0),0x66,99,2);number(target.actor.storage,0x4c,20);
    target.actor.model->animation().set_channel(1,{0,0,60,1});
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(number(b,0xf8)==42.5f&&word(b,0x300)==2,"Diagonal path offsets by half TARGET radius plus7.5");
    //Preserved accelerationW is scaled by actual timestep squared.
    put(b,0x300,1);number(b,0x12c,96);number(b,0x2c,.5f);number(b,0x8c,8);
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(number(b,0x8c)==2&&number(b,0x12c)==95.5f,"Ballistic accelerationW has a real retained owner and scales by dt squared");
    put(b,0x4a,2,2);const float before_failure=number(b,0x74);
    rejects([&]{okage::battle_actor_first_update(a,f.root,entities,services);});
    require(number(b,0x74)!=before_failure&&number(b,0x12c)==95.5f,
            "Phase2 table guard follows actual position and velocity writes, before countdown");
    put(b,0x300,3);number(b,0x12c,0);
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(word(b,0x24)==2&&number(b,0x12c)==-.5f,"Phase3 does not read the unused invalid timing row");
    put(b,0x24,1);put(b,0x300,0);put(b,0x4a,1,2);number(b,0x2c,1);
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(number(b,0x12c)==179&&word(b,0x300)==1,"Authored second timing row starts180");
    number(b,0x12c,120);f.root.bytes[0x60]=0;put(f.root.entity(0),0x66,0xffff,2);
    services.approach_offset_w={};
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(word(b,0x300)==2&&number(b,0x12c)==119&&number(b,0xf8)==60,
            "Negative grouping ordinal skips offset and scratch read at second-row equality");
}
void ballistic_cartesian_equivalence() {
    using Callback=okage::BattleActor::FirstCallback;
    const auto sine=okage::make_sine_table();
    for(float scratch_w:{-3.f,.5f,4.f}) {
        Fixture spatial,exact,spatial_target,exact_target;
        const auto prepare=[](Fixture& f,Fixture& target) {
            auto& a=f.actor;auto& b=a.storage;
            put(b,0x40,1,2);put(b,0x44,23,2);put(b,0x21c,6);b[0x132]=1;
            a.action_motion.emplace();number(a.action_motion->bytes,0,0);
            for(unsigned i=0;i<4;++i) {
                number(b,0x60+4*i,okage::Float4{3,-2,7,1}[i]);
                number(b,0x90+4*i,okage::Float4{0,.25f,0,1}[i]);
                number(b,0xc0+4*i,1);
            }
            number(b,0x4c,10);number(b,0x7c,0);number(b,0x8c,0);
            target.actor.model->animation().set_channel(1,{30,4,-8,1});
            put(f.root.entity(0),0x66,1,2);
        };
        prepare(spatial,spatial_target);prepare(exact,exact_target);
        std::array<okage::BattleEntityBinding,2> spatial_bindings{{{&spatial.actor,spatial.record,1},{&spatial_target.actor,spatial_target.record,0}}};
        std::array<okage::BattleEntityBinding,2> exact_bindings{{{&exact.actor,exact.record,1},{&exact_target.actor,exact_target.record,0}}};
        okage::BattleStrikeServices spatial_services,exact_services;
        unsigned explicit_reads=0;
        exact_services.approach_offset_w=[&](const okage::BattleActor&){++explicit_reads;return scratch_w;};
        okage::battle_actor_motion_update(spatial.actor,spatial_bindings);
        okage::battle_actor_motion_update(exact.actor,exact_bindings);
        bool introduced=false,saw_raw_difference=false,saw_channel_difference=false;
        std::array<bool,4> phases{};unsigned ticks=0;
        do {
            require(++ticks<250,"Cartesian comparison must finish actual callback lifecycle");
            okage::battle_actor_first_update(spatial.actor,spatial.root,spatial_bindings,spatial_services);
            okage::battle_actor_first_update(exact.actor,exact.root,exact_bindings,exact_services);
            auto& a=spatial.actor;auto& b=exact.actor;
            if(word(a.storage,0x24)<phases.size())phases[word(a.storage,0x24)]=true;
            require(a.first_callback==b.first_callback&&a.saved_first_callback==b.saved_first_callback,
                    "Cartesian and explicit-W callbacks restore at identical source boundaries");
            require(a.model->motion()==b.model->motion()&&a.model->animation().script_state()==b.model->animation().script_state(),
                    "Cartesian policy preserves motion selection and script state");
            for(unsigned i=0;i<a.storage.size();++i) {
                if((i>=0x7c&&i<0x80)||(i>=0xfc&&i<0x100))continue;
                require(a.storage[i]==b.storage[i],"Cartesian task scalars and all XYZ lanes remain bit-exact");
            }
            require(spatial.root.bytes==exact.root.bytes&&spatial.record==exact.record,
                    "Cartesian policy changes no root or live stat bytes");
            introduced|=a.cartesian_motion;
            require(!b.cartesian_motion,"Explicit supplied W retains four-lane source mode");
            if(introduced)require(a.cartesian_motion,"Source-W unavailable marker persists after introduction");
            const auto ap=a.model->animation().value(1),bp=b.model->animation().value(1);
            for(unsigned axis=0;axis<3;++axis)
                require(std::bit_cast<std::uint32_t>(ap[axis])==std::bit_cast<std::uint32_t>(bp[axis]),
                        "Every evaluated spatial channel XYZ remains bit-exact");
            require(ap[3]==1,"Cartesian native point has defined homogeneous W1");
            saw_channel_difference|=ap[3]!=bp[3];
            saw_raw_difference|=word(a.storage,0xfc)!=word(b.storage,0xfc)||word(a.storage,0x7c)!=word(b.storage,0x7c);
            a.model->evaluate_world();b.model->evaluate_world();
            require(a.model->world(sine)==b.model->world(sine),"Source-derived world transform is independent of raw position W");
        } while(spatial.actor.first_callback!=Callback::Roster);
        require(introduced&&saw_raw_difference&&saw_channel_difference&&explicit_reads==1,
                "Test distinguishes Cartesian representation from three different supplied raw W values");
        require(phases[1]&&phases[2]&&phases[3]&&word(spatial.actor.storage,0x21c)==99,
                "Comparison covers whole approach, zero-duration strike and return");
        require(spatial.actor.model->animation().value(1)==okage::Float4({3,-2,7,1}),
                "Return restores real baseline XYZ and homogeneous point");
    }
}
void strike_schedules() {
    Fixture f,target,excluded;auto& a=f.actor;auto& b=a.storage;
    std::array<okage::BattleEntityBinding,3> entities{{{&a,f.record,0},{&target.actor,target.record,1},{&excluded.actor,excluded.record,1}}};
    put(f.root.bytes,0x50,3);put(f.root.bytes,0x25c+2*0x200,0x80000000);
    a.first_callback=okage::BattleActor::FirstCallback::ApproachAttack;
    put(b,0x24,2);put(b,0x21c,5);put(b,0x140,2,2);b[0x132]=1;b[0x133]=2;
    a.action_motion.emplace();auto& d=a.action_motion->bytes;number(d,0,10);
    put(d,8,1,2);put(d,10,16,2);put(d,12,1,2);put(d,14,17,2);put(d,16,0xffff,2);
    put(d,0x18,40,2);put(d,0x1a,1,2);put(d,0x1c,41,2);put(d,0x1e,1,2);put(d,0x22,0xffff,2);
    put(d,0x38,38,2);put(d,0x3a,1,2);put(d,0x3c,2);
    number(d,0x40,2);number(d,0x44,-7.5f);number(d,0x48,3);
    put(d,0x4c,39,2);put(d,0x4e,1,2);put(d,0x50,0);number(d,0x54,7);
    put(d,0x62,0xffff,2);
    for(unsigned at:{0x1e0,0x1e8,0x1f0})number(b,at,1);
    target.actor.model->animation().set_channel(1,{10,20,30,5});
    std::vector<int> events;std::vector<okage::Float4> points;std::vector<bool> source_w;
    okage::BattleStrikeServices services;
    services.damage_event=[&](int command,int owner,int recipient){require(owner==0&&recipient==1,"Damage filters disabled target and uses signed owner");events.push_back(command);};
    services.sound=[&](int id){events.push_back(id);};
    services.positioned_effect=[&](int id,const okage::Float4& point,bool exact_w){events.push_back(id);points.push_back(point);source_w.push_back(exact_w);return unsigned(points.size()+10);};
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(events==std::vector<int>({16,40,38}),"Damage then sound then effects, one entry each per callback");
    require(points[0]==okage::Float4({12,12.5f,33,5})&&word(b,0x30c)==11,"Effect uses live target channel plus authored offsets, retains W and returned handle");
    require(a.model->motion()==3&&number(b,0x12c)==9&&number(b,0x1f8)==1&&word(b,0x300)==1,
            "Strike selects motion then immediately advances duration and shared elapsed");
    require(word(b,0x1e4)==1&&word(b,0x1ec)==1&&word(b,0x1f4)==1,"Due subsequent entries wait for next invocation");
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(events==std::vector<int>({16,40,38,17,41,39}),"Each next schedule entry runs on the following callback");
    require(points[1]==okage::Float4({7,0,0,1}),"Absolute effect has authored coordinates and W1");
    require(word(b,0x1e4)==4&&word(b,0x1ec)==8&&word(b,0x1f4)==4,"Negative next times terminate each schedule");
    require(source_w==std::vector<bool>({true,true}),"Exact target and authored absolute effects retain W provenance");
    target.actor.cartesian_motion=true;put(b,0x1f4,0);number(b,0x1f0,0);
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(!source_w.back(),"Targeted effect marks source W unavailable for Cartesian owner");
    put(b,0x30c,12);number(b,0x1f8,2);
    //Failure after sound retains elapsed, sound counter and previous effect handle.
    put(b,0x1ec,0);put(b,0x1f4,0);number(b,0x1e8,1);number(b,0x1f0,1);
    services.positioned_effect=[](int,const okage::Float4&,bool)->unsigned{throw std::runtime_error("synthetic effect service failure");};
    rejects([&]{okage::battle_actor_first_update(a,f.root,entities,services);});
    require(number(b,0x1f8)==3&&word(b,0x1ec)==1&&word(b,0x1f4)==0&&word(b,0x30c)==12,
            "Service failure retains ordered prior stores without consuming effect entry");
    put(b,0x1ec,8);put(d,0x3c,3);
    services.positioned_effect=[](int,const okage::Float4&,bool){return 27u;};
    rejects([&]{okage::battle_actor_first_update(a,f.root,entities,services);});
    require(word(b,0x30c)==27&&word(b,0x1f4)==0,"Missing 101758 service retains preceding spawn and handle store");
    std::vector<std::pair<unsigned,okage::Float4>> rotations;
    services.effect_rotation=[&](unsigned handle,const okage::Float4& r){rotations.push_back({handle,r});};
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(word(b,0x1f4)==1&&rotations.size()==1&&rotations[0].first==27&&rotations[0].second==okage::Float4{0,.5f,0,1},
            "Type3 sets the spawned job rotation to 244048 (0,.5,0) with W=1");
    put(b,0x1f4,0);put(d,0x3c,4);put(b,0x140,0,2);
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(word(b,0x1f4)==1&&word(b,0x30c)==27&&rotations.size()==1,"Type4 with no targets advances without rotation");
    //Equality is not expiry; negative remaining skips elapsed and every service.
    put(b,0x1ec,8);put(b,0x1f4,4);number(b,0x12c,1);number(b,0x1f8,9);
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(number(b,0x12c)==0&&number(b,0x1f8)==10&&word(b,0x24)==2,"Duration equality remains in strike phase");
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(number(b,0x1f8)==10&&word(b,0x300)==0&&word(b,0x24)==3,"Negative duration completes before incrementing elapsed");
}
void channels_and_effect_guards() {
    for(bool visible:{false,true}) {
        Fixture f;f.bone_transition();f.actor.model->visible=visible;
        const auto script=f.actor.model->animation().script_state();
        okage::battle_party_second_update(f.actor,f.root);
        require(f.actor.model->animation().channel_outputs()[4][0]==0,"Single channel step leaves bone evaluation deferred");
        require(f.actor.model->animation().value(4)[0]==.0625f,"Single channel step ignores visibility");
        require(f.actor.model->animation().script_state()==script,"Single step leaves XMT script wait untouched");
    }
    for(bool visible:{false,true}) {
        Fixture f;f.bone_transition();f.actor.model->visible=visible;number(f.actor.storage,0x2c,2);
        const auto script=f.actor.model->animation().script_state();
        f.actor.model->animation().set_channel(0,{91,92,93,1});const auto world=f.actor.model->animation().channel_outputs()[0];
        okage::battle_party_second_update(f.actor,f.root);
        require(f.actor.model->animation().channel_outputs()[4][0]==(visible?.125f:0),"Double pass performs visible-only channel advancement and bone evaluation");
        require(f.actor.model->channels()[4][0]==(visible?.125f:1),"Bone-only cached model output updated on visible double pass");
        require(f.actor.model->animation().channel_outputs()[0]==world,"Bone evaluation keeps world values deferred");
        require(f.actor.model->animation().script_state()==script,"Double pass does not run script");
    }
    Fixture f;f.bone_transition();number(f.actor.storage,0x2c,.25f);
    for(unsigned i=0;i<3;++i)okage::battle_party_second_update(f.actor,f.root);
    require(number(f.actor.storage,0x11c)==.75f&&f.actor.model->animation().value(4)[0]==0,"Fractional timestep accumulates without channel step");
    okage::battle_party_second_update(f.actor,f.root);
    require(word(f.actor.storage,0x11c)==0&&f.actor.model->animation().value(4)[0]==.0625f,"Accumulator threshold triggers one step and resets");
    put(f.actor.storage,0x20,1);f.record[0]=2;put(f.record,0x26,0,2);
    rejects([&]{okage::battle_party_second_update(f.actor,f.root);});
    require(half(f.record,0x26)==25,"Status guard occurs after genuine charge publication");
    f.record[0]=1;put(f.actor.storage,0x310,1);rejects([&]{okage::battle_party_second_update(f.actor,f.root);});
    put(f.actor.storage,0x310,0);f.record[1]=1;rejects([&]{okage::battle_party_second_update(f.actor,f.root);});
    f.record[1]=0;put(f.actor.storage,0x320,1);rejects([&]{okage::battle_party_second_update(f.actor,f.root);});
    put(f.actor.storage,0x320,0);put(f.actor.storage,0x17c,1);rejects([&]{okage::battle_party_second_update(f.actor,f.root);});
    put(f.actor.storage,0x17c,0);put(f.actor.storage,0x330,1);rejects([&]{okage::battle_party_second_update(f.actor,f.root);});
    put(f.actor.storage,0x330,0);f.actor.stat_bindings[1]={};rejects([&]{okage::battle_party_second_update(f.actor,f.root);});
}
void enemy_paths() {
    Fixture f;f.actor.enemy=true;f.bone_transition();put(f.actor.storage,0x20,99);
    okage::battle_enemy_first_update(f.actor,f.root);require(word(f.actor.storage,0x20)==0,"Enemy first clears source marker");
    // Enemy second neither reads timer/charge nor runs party secondary effects.
    put(f.actor.storage,0x194,0x7fc00000);put(f.actor.storage,0x198,0);
    put(f.record,0x26,0xa5a5,2);f.actor.stat_bindings[3]={};put(f.actor.storage,0x17c,1);put(f.actor.storage,0x320,1);
    okage::battle_enemy_second_update(f.actor,f.root);
    require(half(f.record,0x26)==0xa5a5&&f.actor.model->animation().value(4)[0]==.0625f,"Enemy suffix omits party charge and extra effects");
    put(f.root.bytes,0x81b4,4);const auto before=f.actor.storage;
    rejects([&]{okage::battle_enemy_second_update(f.actor,f.root);});require(f.actor.storage==before,"Enemy callback transition guarded before work");
    put(f.root.bytes,0x81b4,0);put(f.root.bytes,0x1d2,1,2);put(f.actor.storage,0x20,99);
    rejects([&]{okage::battle_enemy_first_update(f.actor,f.root);});
    require(word(f.actor.storage,0x20)==0,"Enemy source clear precedes status rejection");
}
void floating_environment() {
    std::fenv_t original;std::fegetenv(&original);
    for(auto mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        Fixture f;put(f.actor.storage,0x21c,1);number(f.actor.storage,0x194,-1);number(f.actor.storage,0x198,100);
        put(f.actor.storage,0x2c,0x3f800001);put(f.actor.storage,0x1a4,0x3f7ffffe);
        std::fesetround(mode);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO|FE_OVERFLOW);
        const auto flags=std::fetestexcept(FE_ALL_EXCEPT);
        okage::battle_party_first_update(f.actor,f.root);
        require(word(f.actor.storage,0x194)==0xb3800000,"MUL.S then ADD.S stays separate and toward zero");
        require(std::fegetround()==mode&&std::fetestexcept(FE_ALL_EXCEPT)==flags,"First callback restores full fenv");
        number(f.actor.storage,0x194,1);number(f.actor.storage,0x198,3);put(f.actor.storage,0x20,1);
        okage::battle_party_second_update(f.actor,f.root);require(half(f.record,0x26)==33,"Charge is independent of caller rounding");
        require(std::fegetround()==mode&&std::fetestexcept(FE_ALL_EXCEPT)==flags,"Second callback restores full fenv");
        put(f.actor.storage,0x2c,0x7f7fffff);number(f.actor.storage,0x1a4,2);
        rejects([&]{okage::battle_party_first_update(f.actor,f.root);});
        require(std::fegetround()==mode&&std::fetestexcept(FE_ALL_EXCEPT)==flags,"Overflow guard restores host flags/mode");
        put(f.actor.storage,0x2c,0x00800000);number(f.actor.storage,0x1a4,.5f);
        rejects([&]{okage::battle_party_first_update(f.actor,f.root);});
        require(std::fegetround()==mode&&std::fetestexcept(FE_ALL_EXCEPT)==flags,"Subnormal guard restores host flags/mode");
    }
    std::fesetenv(&original);
}
void victory_status_reset() {
    for(unsigned primary:{0u,1u,4u,7u,128u,255u}) {
        Fixture f;f.record[0]=primary;f.actor.stat_bindings[2]=std::span(f.record).subspan(0x4a,2);
        put(f.record,0x4a,0xabcd,2);f.record[0x49]=0x56;f.record[0x4c]=0x78;
        const auto task=f.actor.storage;auto expected=f.record;expected[0]=1;expected[0x4a]=expected[0x4b]=0;
        okage::battle_actor_reset_after_victory(f.actor);
        require(f.record==expected&&f.actor.storage==task,"Victory reset writes only live primary/counter, including negative primary");
    }
    Fixture dead;dead.record[0]=6;put(dead.record,0x4a,0xabcd,2);
    const auto record=dead.record;dead.actor.stat_bindings[2]={};
    okage::battle_actor_reset_after_victory(dead.actor);
    require(dead.record==record,"Primary6 preserves counter and does not require its binding");
    for(unsigned offset:{0u,1u}) {
        Fixture alias;std::array<std::uint8_t,2> shared{9,9};
        alias.actor.stat_bindings[1]=std::span(shared).subspan(offset,1);alias.actor.stat_bindings[2]=shared;
        okage::battle_actor_reset_after_victory(alias.actor);
        require(shared==std::array<std::uint8_t,2>{0,0},"Aliased counter clear follows and can overwrite primary1");
    }
    Fixture short_counter;short_counter.record[0]=255;std::array<std::uint8_t,1> short_bytes{0xab};
    short_counter.actor.stat_bindings[2]=short_bytes;
    rejects([&]{okage::battle_actor_reset_after_victory(short_counter.actor);});
    require(short_counter.record[0]==1&&short_bytes[0]==0xab,"Short counter failure retains preceding primary store without partial halfword");
    Fixture missing;std::array<std::uint8_t,2> counter{0xab,0xcd};missing.actor.stat_bindings[1]={};missing.actor.stat_bindings[2]=counter;
    rejects([&]{okage::battle_actor_reset_after_victory(missing.actor);});
    require(counter==std::array<std::uint8_t,2>{0xab,0xcd},"Missing primary rejects before counter mutation");
}
void victory_actor_states() {
    for(unsigned outcome:{0u,1u,3u,0xffffffffu}) {
        Fixture f;put(f.actor.storage,0x21c,100);put(f.actor.storage,0x20,0xbeef);
        put(f.actor.storage,0x128,7);number(f.actor.storage,0x12c,17);put(f.root.bytes,0x81b8,outcome);
        const auto script=f.actor.model->animation().script_state();
        okage::battle_actor_motion_update(f.actor,{},&f.root);
        require(word(f.actor.storage,0x20)==0&&word(f.actor.storage,0x21c)==100&&word(f.actor.storage,0x128)==7&&number(f.actor.storage,0x12c)==17,
                "State100 waits on exact live outcome2 without changing motion or duration");
        require(f.actor.model->animation().script_state()==script,"Waiting state100 does not advance script");
        require(f.actor.model->animation().value(1)[3]==number(f.actor.storage,0x6c),"Waiting state100 publishes common world channel W");
    }
    Fixture f;put(f.actor.storage,0x21c,100);put(f.actor.storage,0x20,99);put(f.root.bytes,0x81b8,2);
    rejects([&]{okage::battle_actor_motion_update(f.actor);});
    require(word(f.actor.storage,0x20)==0&&word(f.actor.storage,0x21c)==100,"Missing state100 root rejects after source TASK20 clear");
    okage::battle_actor_first_update(f.actor,f.root,{});
    require(word(f.actor.storage,0x128)==13&&f.actor.model->motion()==13&&number(f.actor.storage,0x12c)==360&&word(f.actor.storage,0x21c)==102,
            "Party first callback supplies live root and selects victory13 duration360 before state102");
    put(f.actor.storage,0x344,0xffffffff);
    okage::battle_actor_first_update(f.actor,f.root,{});
    require(number(f.actor.storage,0x12c)==359&&word(f.actor.storage,0x21c)==102,"State102 does not restart victory motion; party prefix keeps its timer");
    put(f.actor.storage,0x20,99);const auto script=f.actor.model->animation().script_state();
    okage::battle_actor_motion_update(f.actor);
    require(word(f.actor.storage,0x20)==0&&f.actor.model->animation().script_state()==script&&number(f.actor.storage,0x12c)==359,
            "State102 common tail requires no root and does not advance script or timer");
    Fixture enemy;enemy.actor.enemy=true;put(enemy.actor.storage,0x21c,100);put(enemy.root.bytes,0x81b8,2);
    okage::battle_enemy_first_update(enemy.actor,enemy.root);
    require(word(enemy.actor.storage,0x21c)==102&&enemy.actor.model->motion()==13,"Enemy first wrapper also supplies actual root");
    Fixture failed;put(failed.actor.storage,0x21c,100);put(failed.root.bytes,0x81b8,2);number(failed.actor.storage,0x12c,17);failed.actor.model.reset();
    rejects([&]{okage::battle_actor_motion_update(failed.actor,{},&failed.root);});
    require(word(failed.actor.storage,0x128)==13&&number(failed.actor.storage,0x12c)==17&&word(failed.actor.storage,0x21c)==100,
            "Model failure preserves motion-ID store but precedes duration/state writes");
    Fixture unknown;put(unknown.actor.storage,0x21c,103);
    rejects([&]{okage::battle_actor_motion_update(unknown.actor,{},&unknown.root);});
    require(word(unknown.actor.storage,0x20)==0,"Bounded recovery does not accept unrelated unknown states");
}
void queued_party_hit() {
    const auto prepare=[](Fixture& f,int amount=9) {
        auto& a=f.actor;
        a.stat_bindings[0]=std::span(f.record).subspan(8,4);
        a.stat_bindings[2]=std::span(f.record).subspan(0x4a,2);
        a.stat_bindings[4]=std::span(f.record).subspan(0x4c,2);
        a.stat_bindings[5]=std::span(f.record).subspan(0x4e,2);
        put(f.record,8,40);number(a.storage,0x4c,5);put(a.storage,0x344,0xffffffff);
        auto e=f.root.entity(0);put(e,0x172,2,2);
        put(e,0x17c,1,2);put(e,0x17e,3,2);
        put(e,0x184,0,2);put(e,0x186,amount,2);put(e,0x188,0xfffe,2);put(e,0x18a,0xabcd,2);
    };
    std::vector<int> events;
    okage::BattleHitServices services{
        [&](int id,okage::BattleActor&){events.push_back(id);return 4u;},
        [&](unsigned,int amount){events.push_back(amount);},
        [&](okage::BattleActor&,std::int16_t delta,std::int16_t){events.push_back(delta);},
        [&](okage::BattleActor&){events.push_back(200);},
        [&](int id,const okage::Float4&,bool){events.push_back(id);return 5u;}};
    Fixture f;prepare(f);put(f.actor.storage,0x3bc,0xffff,2);
    okage::battle_actor_first_update(f.actor,f.root,{}, {},&services);
    auto e=f.root.entity(0);
    require(events==std::vector<int>({35,9,-9,38})&&word(f.record,8)==31,"Party uses shared hit effects/history and HP semantics");
    require(half(f.actor.storage,0x3bc)==0&&half(f.actor.storage,0x148)==0xfffe,"Party count wraps and signed attacker owner is retained");
    require(half(e,0x172)==1&&half(e,0x184)==0xffff&&half(e,0x186)==0&&half(e,0x188)==0&&half(e,0x18a)==0xabcd&&half(e,0x17e)==3,
            "Party consumes exactly the newest event and preserves padding/older event");
    require(number(f.actor.storage,0x12c)==29&&f.actor.model->motion()==4,"Party reaction decrements in the same first callback");
    struct CounterCase {unsigned old,base,counter,remaining;};
    for(const auto c:{CounterCase{998,1200,999,201},CounterCase{999,1200,999,201},
                      CounterCase{32767,20,32768,0},CounterCase{65535,20,0,20},
                      CounterCase{0,0,1,0},CounterCase{0,32769,1,0}}) {
        Fixture x;prepare(x);x.record[1]=4;put(x.record,0x4c,c.old,2);put(x.record,0x1c,c.base,2);
        put(x.actor.storage,0x168,0xbeef,2);put(x.actor.storage,0x128,77);
        okage::battle_party_first_update(x.actor,x.root,{},&services);
        require(half(x.record,0x4c)==c.counter&&half(x.actor.storage,0x168)==c.remaining,"Status4 uses signed narrowed counter and signed narrowed subtraction");
        require(word(x.actor.storage,0x128)==4,"Status4 derived stat writes TASK168, not motion TASK128");
    }
    Fixture alias;prepare(alias);alias.record[1]=4;
    alias.actor.stat_bindings[4]=std::span(alias.record).subspan(0x1c,2);put(alias.record,0x1c,20,2);
    put(alias.root.entity(0),0x184,1,2);
    okage::battle_party_first_update(alias.actor,alias.root,{},&services);
    require(half(alias.record,0x1c)==21&&half(alias.actor.storage,0x168)==0,"Category1 and aliased counter read source baseline after increment store");
    for(unsigned cursor:{16u,65535u}) {
        Fixture invalid;prepare(invalid);put(invalid.root.entity(0),0x172,cursor,2);events.clear();
        rejects([&]{okage::battle_party_first_update(invalid.actor,invalid.root,{},&services);});
        require(events.empty()&&half(invalid.root.entity(0),0x172)==cursor,"Invalid signed queue cursor rejects before hit services or stores");
    }
    Fixture miss;prepare(miss,0);events.clear();put(miss.actor.storage,0x148,33,2);
    okage::battle_party_first_update(miss.actor,miss.root,{},&services);
    require(events==std::vector<int>({36})&&word(miss.record,8)==40&&half(miss.actor.storage,0x3bc)==0&&half(miss.actor.storage,0x148)==33,
            "Nonpositive miss pops without counting a hit or replacing attacker");
    Fixture moved;prepare(moved);auto movedqueue=moved.root.entity(1);
    put(movedqueue,0x172,1,2);put(movedqueue,0x17c,1,2);put(movedqueue,0x17e,88,2);put(movedqueue,0x180,23,2);
    auto mutable_services=services;mutable_services.positioned_effect=[&](int,const okage::Float4&,bool) {
        put(moved.actor.storage,0x130,1,2);return 1u;
    };
    okage::battle_party_first_update(moved.actor,moved.root,{},&mutable_services);
    require(half(moved.actor.storage,0x148)==23&&half(movedqueue,0x172)==0&&half(moved.root.entity(0),0x172)==2,
            "Party reloads entity and cursor after mutable hit services");
    Fixture failure;prepare(failure);auto fail_services=services;
    fail_services.positioned_effect=[](int,const okage::Float4&,bool)->unsigned {throw std::runtime_error("Interrupted impact");};
    rejects([&]{okage::battle_party_first_update(failure.actor,failure.root,{},&fail_services);});
    require(word(failure.record,8)==31&&number(failure.actor.storage,0x12c)==30&&half(failure.root.entity(0),0x172)==2&&half(failure.actor.storage,0x3bc)==0,
            "Impact failure retains HP/motion but precedes party counters, pop and timer");
    Fixture unsupported;prepare(unsupported);put(unsupported.root.entity(0),0x184,2,2);
    rejects([&]{okage::battle_party_first_update(unsupported.actor,unsupported.root,{},&services);});
    require(word(unsupported.record,8)==40&&half(unsupported.root.entity(0),0x172)==2,"Unsupported category never silently pops");
    Fixture lethal;prepare(lethal,40);put(lethal.record,0x4a,12,2);lethal.record[1]=4;put(lethal.record,0x4c,7,2);put(lethal.record,0x4e,9,2);put(lethal.actor.storage,0x30,17);put(lethal.root.entity(0),0x60,17,2);put(lethal.root.entity(0),0x68,9);
    okage::battle_party_first_update(lethal.actor,lethal.root,{},&services);
    require(word(lethal.record,8)==0&&lethal.record[0]==6&&half(lethal.record,0x4a)==0&&half(lethal.root.entity(0),0x60)==0xffff&&word(lethal.root.entity(0),0x68)==0,
            "Lethal installation retains preceding HP, graph and live status writes");
    require(half(lethal.root.entity(0),0x172)==0&&lethal.actor.dead_callback&&word(lethal.actor.storage,0x21c)==99,
            "Lethal caller clears source queue after installing death callback");
    require(lethal.record[1]==0&&half(lethal.record,0x4c)==0&&half(lethal.record,0x4e)==0&&word(lethal.actor.storage,0x30)==0,
            "153550 clears live secondary status and both counters before death callback installation");
    require(lethal.actor.first_callback==okage::BattleActor::FirstCallback::PartyDeath&&
            lethal.actor.saved_first_callback==okage::BattleActor::FirstCallback::Roster&&word(lethal.actor.storage,0x24)==0&&half(lethal.actor.storage,0x142)==200,
            "Lethal caller saves callback and initializes phase/action before returning to common motion tail");
    require(half(lethal.root.entity(0),0x174)==0xffff&&half(lethal.root.entity(0),0x17c)==0xffff&&half(lethal.root.entity(0),0x184)==0xffff&&half(lethal.root.entity(0),0x18a)==0xabcd,
            "Lethal queue clear visits newest through slot0 and preserves padding");
    require((word(lethal.root.entity(0),0x1fc)&0x80000000u)==0,"Party lethal caller does not use enemy inactive flag store");
    Fixture stop;prepare(stop,40);put(stop.actor.storage,0x344,3);put(stop.actor.storage,0x20,77);
    rejects([&]{okage::battle_party_first_update(stop.actor,stop.root,{},&services);});
    require(stop.actor.first_callback==okage::BattleActor::FirstCallback::PartyDeath&&stop.actor.dead_callback&&
            half(stop.root.entity(0),0x172)==2&&word(stop.actor.storage,0x20)==77&&half(stop.actor.storage,0x142)==200,
            "Unsupported lethal effect stop rejects after installation but before queue reset and common tail");
    for(unsigned primary:{0u,1u,2u,5u,6u,255u}) {
        Fixture expiry;put(expiry.actor.storage,0x344,0xffffffff);expiry.record[0]=primary;number(expiry.actor.storage,0x12c,0);
        okage::battle_party_first_update(expiry.actor,expiry.root);
        require(expiry.actor.model->motion()==(primary>=2&&primary<=5?5u:1u)&&number(expiry.actor.storage,0x12c)==-1,
                "Party expiry selects motion from signed primary status");
    }
    Fixture special;put(special.actor.storage,0x344,0xffffffff);put(special.actor.storage,0x184,1);number(special.actor.storage,0x12c,0);
    special.actor.stat_bindings[1]={};
    okage::battle_party_first_update(special.actor,special.root);
    require(special.actor.model->motion()==14&&number(special.actor.storage,0x12c)==-1,"Special expiry selects14 without reading primary binding");
}
void party_death_lifecycle() {
    using Callback=okage::BattleActor::FirstCallback;
    const auto prepare=[](Fixture& f) {
        f.actor.first_callback=Callback::PartyDeath;f.actor.dead_callback=true;
        f.actor.saved_first_callback=Callback::Roster;f.record[0]=6;
        put(f.actor.storage,0x180,17);put(f.actor.storage,0x17c,19);
        put(f.actor.storage,0x128,77);number(f.actor.storage,0x12c,88);
    };
    okage::BattleStrikeServices services;unsigned refreshes=0;
    services.refresh_hud=[&]{++refreshes;};
    constexpr float durations[]={40,100,120,30,240,30,-1,0};
    for(unsigned id=0;id<8;++id) {
        Fixture f;prepare(f);put(f.actor.storage,0x44,id,2);f.bone_transition();
        const auto world=f.actor.model->animation().value(1);
        okage::battle_actor_first_update(f.actor,f.root,{},services);
        require(word(f.actor.storage,0x20)==3&&word(f.actor.storage,0x180)==0&&word(f.actor.storage,0x17c)==0,
                "Death entry writes3 and clears TASK180/17c");
        require(f.actor.model->motion()==(id<6?10u:9u)&&word(f.actor.storage,0x24)==(id<6?1u:2u)&&
                number(f.actor.storage,0x12c)==(id<6?durations[id]-1:-1),
                "All eight death table entries fall through same-callback strict-negative timer");
        require(f.actor.model->animation().value(1)==world&&f.actor.model->animation().value(4)[0]>0,
                "Death tail advances real bone channel without publishing ordinary world channel");
    }
    require(refreshes==8,"Every entry invokes HUD refresh exactly once");
    Fixture f;prepare(f);okage::battle_actor_first_update(f.actor,f.root,{},services);
    number(f.actor.storage,0x12c,1);okage::battle_actor_first_update(f.actor,f.root,{},services);
    require(word(f.actor.storage,0x24)==1&&number(f.actor.storage,0x12c)==0,"Death timer equality does not advance");
    okage::battle_actor_first_update(f.actor,f.root,{},services);
    require(word(f.actor.storage,0x24)==2&&f.actor.model->motion()==9&&number(f.actor.storage,0x12c)==-1,"Negative timer installs indefinite dead motion");
    const auto count=refreshes;okage::battle_actor_first_update(f.actor,f.root,{},services);
    require(refreshes==count&&word(f.actor.storage,0x24)==2&&number(f.actor.storage,0x12c)==-1,"Empty dead queue performs only channel tail");
    auto e=f.root.entity(0);put(e,0x172,1,2);put(e,0x17c,0,2);put(e,0x17e,4,2);
    okage::BattleHitServices hits;
    rejects([&]{okage::battle_actor_first_update(f.actor,f.root,{},services,&hits);});
    require(half(e,0x172)==1&&half(e,0x17e)==4&&word(f.actor.storage,0x24)==2,"Unrecovered dead hit remains queued at shared primary6 guard");
    Fixture pop;prepare(pop);put(pop.actor.storage,0x24,2);pop.record[0]=1;
    auto oldqueue=pop.root.entity(0),newqueue=pop.root.entity(1);
    put(oldqueue,0x172,1,2);put(oldqueue,0x17c,0,2);put(oldqueue,0x17e,0,2);
    put(newqueue,0x172,1,2);put(newqueue,0x17c,7,2);put(newqueue,0x17e,8,2);put(newqueue,0x180,9,2);put(newqueue,0x182,0xabcd,2);
    okage::BattleHitServices moved_hits;
    moved_hits.attached_effect=[&](int id,okage::BattleActor& target){require(id==36,"Death ordinary miss uses shared effect");put(target.storage,0x130,1,2);return 0u;};
    okage::battle_actor_first_update(pop.actor,pop.root,{},services,&moved_hits);
    require(half(oldqueue,0x172)==1&&half(newqueue,0x172)==0&&half(newqueue,0x17c)==0xffff&&half(newqueue,0x17e)==0&&half(newqueue,0x180)==0&&half(newqueue,0x182)==0xabcd,
            "Death queue pop reloads live entity after services and preserves slot padding");
    // Independently source-check phase3; no fabricated revival result drives it.
    put(f.actor.storage,0x24,3);number(f.actor.storage,0x12c,1);put(f.actor.storage,0x34,99);
    put(f.actor.storage,0x130,2,2);put(f.root.entity(2),0x1fc,0x81234567);put(e,0x1fc,0x87654321);
    okage::battle_actor_first_update(f.actor,f.root,{},services);
    require(f.actor.first_callback==Callback::PartyDeath&&word(f.root.entity(2),0x1fc)==0x81234567,"Revival timer equality preserves callback and flags");
    okage::battle_actor_first_update(f.actor,f.root,{},services);
    require(f.actor.first_callback==Callback::Roster&&!f.actor.dead_callback&&word(f.actor.storage,0x24)==0&&word(f.actor.storage,0x34)==0&&
            f.actor.model->motion()==1&&number(f.actor.storage,0x12c)==-1&&word(f.root.entity(2),0x1fc)==0x01234567&&word(e,0x1fc)==0x87654321,
            "Revival expiry uses live entity then restores callback before idle motion");
    Fixture missing;prepare(missing);
    rejects([&]{okage::battle_actor_first_update(missing.actor,missing.root,{});});
    require(word(missing.actor.storage,0x20)==3&&word(missing.actor.storage,0x128)==77&&word(missing.actor.storage,0x180)==17,"Missing refresh rejects after only entry task20 store");
    auto failed_refresh=services;
    failed_refresh.refresh_hud=[&]{++refreshes;throw std::runtime_error("Interrupted refresh");};
    rejects([&]{okage::battle_actor_first_update(missing.actor,missing.root,{},failed_refresh);});
    require(word(missing.actor.storage,0x24)==0&&word(missing.actor.storage,0x128)==77&&word(missing.actor.storage,0x180)==17,
            "Failed HUD refresh cannot select motion or clear flags");
    for(unsigned id:{65535u,8u}) {
        Fixture invalid;prepare(invalid);put(invalid.actor.storage,0x44,id,2);auto before=refreshes;
        rejects([&]{okage::battle_actor_first_update(invalid.actor,invalid.root,{},services);});
        require(refreshes==before+1&&word(invalid.actor.storage,0x128)==77&&word(invalid.actor.storage,0x180)==17,"Signed table guard follows source refresh and precedes motion");
    }
    Fixture failed;prepare(failed);failed.actor.model.reset();
    rejects([&]{okage::battle_actor_first_update(failed.actor,failed.root,{},services);});
    require(word(failed.actor.storage,0x128)==10&&number(failed.actor.storage,0x12c)==88&&word(failed.actor.storage,0x180)==17,"Model failure retains selected motion store before duration and clears");
    Fixture restored;prepare(restored);put(restored.actor.storage,0x24,3);number(restored.actor.storage,0x12c,0);
    put(restored.root.entity(0),0x1fc,0x80000003);put(restored.actor.storage,0x34,99);restored.actor.saved_first_callback.reset();
    rejects([&]{okage::battle_actor_first_update(restored.actor,restored.root,{},services);});
    require(number(restored.actor.storage,0x12c)==-1&&word(restored.root.entity(0),0x1fc)==3&&word(restored.actor.storage,0x34)==0&&word(restored.actor.storage,0x24)==3,
            "Missing saved callback preserves preceding timer, active flag and task34 writes");
    Fixture rounding;prepare(rounding);put(rounding.actor.storage,0x24,1);number(rounding.actor.storage,0x12c,1);number(rounding.actor.storage,0x2c,0x1p-25f);
    const int old_round=std::fegetround();std::fesetround(FE_UPWARD);
    okage::battle_actor_first_update(rounding.actor,rounding.root,{},services);
    require(word(rounding.actor.storage,0x12c)==0x3f7fffffu&&std::fegetround()==FE_UPWARD,"Death subtraction is separate toward-zero single and restores host rounding");
    std::fesetround(old_round);
    Fixture other;prepare(other);put(other.actor.storage,0x24,99);other.bone_transition();
    okage::battle_actor_first_update(other.actor,other.root,{});
    require(word(other.actor.storage,0x24)==99&&word(other.actor.storage,0x20)==3&&other.actor.model->animation().value(4)[0]>0,"Source default phase only advances channel tail");
}
void queued_enemy_hit() {
    Fixture f;auto& a=f.actor;a.enemy=true;
    a.stat_bindings[0]=std::span(f.record).subspan(8,4);put(f.record,8,40);
    number(a.storage,0x4c,5);put(f.root.bytes,0x50,1);
    auto e=f.root.entity(0);put(e,0x172,2,2);
    put(e,0x17c,0,2);put(e,0x17e,3,2); //Earlier event must survive one callback.
    put(e,0x184,0,2);put(e,0x186,9,2);put(e,0x188,7,2);put(e,0x18a,0xabcd,2);
    std::vector<int> events;
    okage::BattleHitServices services{
        [&](int id,okage::BattleActor&){events.push_back(id);return 4u;},
        [&](unsigned handle,int amount){require(handle==4&&amount==9,"Queued amount reaches number effect");events.push_back(amount);},
        [&](okage::BattleActor&,std::int16_t delta,std::int16_t category){require(delta==-9&&category==0,"Queued category and negative history");events.push_back(100);},
        [&](okage::BattleActor&){events.push_back(200);},
        [&](int id,const okage::Float4&,bool){events.push_back(id);return 5u;}};
    okage::battle_enemy_first_update(a,f.root,{},&services);
    require(events==std::vector<int>({35,9,100,38})&&word(f.record,8)==31,"Source-ordered hit services and HP update");
    require(half(e,0x172)==1&&half(e,0x184)==0xffff&&half(e,0x186)==0&&half(e,0x188)==0&&half(e,0x18a)==0xabcd,
            "One LIFO event cleared; unused fourth halfword survives");
    require(half(e,0x17e)==3&&number(a.storage,0x12c)==29&&a.model->motion()==4,"Earlier event retained and new reaction timer decremented same callback");
    put(e,0x172,0,2);number(a.storage,0x12c,0);
    okage::battle_enemy_first_update(a,f.root,{},&services);
    require(a.model->motion()==1&&number(a.storage,0x12c)==-1,"Reaction expiry restores normal motion immediately");
    put(e,0x172,2,2);put(e,0x184,0,2);put(e,0x186,9,2);put(f.record,8,9);
    okage::battle_enemy_first_update(a,f.root,{},&services);
    require(word(f.record,8)==0&&f.record[0]==6&&a.first_callback==okage::BattleActor::FirstCallback::EnemyDeath,
            "Lethal hit changes HP/status and installs actual death callback");
    require(a.saved_first_callback==okage::BattleActor::FirstCallback::Roster&&word(a.storage,0x21c)==0xffffffff&&word(a.storage,0x24)==0,
            "Death preserves prior callback and resets phase");
    require(half(e,0x172)==0&&half(e,0x174)==0xffff&&half(e,0x17c)==0xffff&&half(e,0x184)==0xffff&&half(e,0x18a)==0xabcd,
            "Death resets active queue slots through zero without touching padding");
    require((word(e,0x1fc)&0x80000000)!=0,"Death marks source target inactive");
    a.model->render_flags=0x41;
    const auto position=a.model->animation().value(1);
    okage::battle_actor_first_update(a,f.root,{}, {},&services);
    require(word(a.storage,0x20)==3&&word(a.storage,0x24)==1,"Death installs fade without phase fallthrough");
    require(a.model->render_flags==0x100041&&a.model->shadow_mode_override==0xffff,"Death preserves model flags and disables shadow override");
    require(a.model->animation().moving(3,0)&&number(a.storage,0xec)==0,"Death fades real color channel");
    require(a.model->animation().value(1)==position,"Death tail does not republish baseline transforms");
    for(unsigned i=0;i<59;++i)okage::battle_actor_first_update(a,f.root,{}, {},&services);
    require(word(a.storage,0x24)==1&&a.model->animation().moving(3,0),"Rounded duration keeps the real channel pending at 60 advances");
    for(unsigned i=0;i<2;++i)okage::battle_actor_first_update(a,f.root,{}, {},&services);
    require(word(a.storage,0x24)==1&&!a.model->animation().moving(3,0),"Death completion is polled on next callback");
    events.clear();
    okage::BattleStrikeServices sounds;sounds.sound=[&](int id){events.push_back(id);};
    okage::battle_actor_first_update(a,f.root,{},sounds,&services);
    require(events==std::vector<int>({20,66})&&!a.model->visible&&word(a.storage,0x24)==2&&number(a.storage,0x3c4)==30,
            "Death creates attached effect then sound before hiding and starting reward delay");
    number(a.storage,0x3c4,1);okage::battle_actor_first_update(a,f.root,{},sounds,&services);
    require(number(a.storage,0x3c4)==0,"Reward delay equality does not complete");
    rejects([&]{okage::battle_actor_first_update(a,f.root,{},sounds,&services);});
    require(number(a.storage,0x3c4)==-1,"Missing reward service retains preceding timer subtraction");
    events.clear();
    sounds.enemy_reward=[&](okage::BattleActor& target) {
        require(&target==&a&&word(a.storage,0x24)==2&&number(a.storage,0x3c4)==-2,
                "Reward service follows strict-negative subtraction before phase99");
        events.push_back(300);
    };
    sounds.sound=[&](int id){events.push_back(id);throw std::runtime_error("Sound test interruption");};
    rejects([&]{okage::battle_actor_first_update(a,f.root,{},sounds,&services);});
    require(events==std::vector<int>({300,65})&&word(a.storage,0x24)==2,"Reward precedes sound and failed sound preserves phase2");
    sounds.enemy_reward=[&](okage::BattleActor&){events.push_back(300);};
    sounds.sound=[&](int id){events.push_back(id);};events.clear();
    okage::battle_actor_first_update(a,f.root,{},sounds,&services);
    require(events==std::vector<int>({300,65})&&word(a.storage,0x24)==99,"Successful reward and sound advance phase99");
    events.clear();okage::battle_actor_first_update(a,f.root,{},sounds,&services);
    require(events.empty(),"Completed death callback does not award repeatedly");
}
}
namespace {
using Callback=okage::BattleActor::FirstCallback;
template<class F> void rejects_with(F action,const char* text) {
    try{action();}catch(const std::runtime_error& e){
        require(std::string(e.what()).find(text)!=std::string::npos,"Guard message names the unrecovered source boundary");return;}
    throw std::runtime_error(std::string("Expected rejection: ")+text);
}
struct SequenceFixture {
    okage::BattleState root;
    std::array<Fixture,14> actors;
    std::vector<okage::BattleEntityBinding> entities;
    SequenceFixture():entities(14) {
        root.initialize_root({1,10,1});put(root.bytes,0x50,14);
        for(unsigned i=0;i<14;++i) {
            auto& a=actors[i].actor;a.entity_id=i;put(a.storage,0x130,i,2);
            entities[i]={&a,actors[i].record,i<4?0u:1u};
            put(root.entity(i),0x1fc,i>=4&&i!=6&&i!=8?0x80000000u:0u);
        }
        stan().stat_bindings[0]=std::span(actors[3].record).subspan(8,4);
        put(stan().storage,0x42,3,2);put(stan().storage,0x44,6,2);
    }
    okage::BattleActor& stan(){return actors[3].actor;}
    void targets(std::initializer_list<int> list) {
        for(unsigned i=0;i<10;++i)stan().storage[0x132+i]=0xff;
        unsigned n=0;for(int t:list)stan().storage[0x132+n++]=std::uint8_t(t);
        put(stan().storage,0x140,n,2);
    }
    void installed() {
        auto& s=stan();s.first_callback=Callback::ActionSequence;s.saved_first_callback=Callback::Roster;
        put(s.storage,0x30,3);put(s.storage,0x24,0);
    }
};
void state8_install_guards() {
    {
        //return before154dac and before the154e20 channel writes.
        SequenceFixture f;auto& s=f.stan();f.targets({6,8});f.entities[8].actor=nullptr;
        put(s.storage,0x21c,8);put(s.storage,0x24,19);put(s.storage,0x30,77);put(s.storage,0x20,5);
        s.model->animation().set_channel(1,{9,9,9,1});
        std::vector<std::string> logs;
        okage::battle_party_first_update(s,f.root,f.entities,nullptr,[&](std::string_view t){logs.emplace_back(t);});
        require(logs==std::vector<std::string>{"OBJ:3 TGT:8 CAUTION!! ILLEGAL TARGET NUMBER!!!!\n"},
                "Missing target logs the pinned 243e70 text with owner and target");
        require(s.first_callback==Callback::Roster&&!s.saved_first_callback&&word(s.storage,0x24)==19&&
                word(s.storage,0x30)==77&&word(s.storage,0x20)==0&&word(s.storage,0x21c)==8,
                "Missing target returns after the TASK20 clear and before the install");
        require(s.model->animation().value(1)==okage::Float4({9,9,9,1}),"Early return skips the three 1b4220 writes");
        f.entities[8].actor=&f.actors[8].actor;f.entities[6].actor=nullptr;logs.clear();
        okage::battle_party_first_update(s,f.root,f.entities,nullptr,[&](std::string_view t){logs.emplace_back(t);});
        require(logs==std::vector<std::string>{"OBJ:3 TGT:6 CAUTION!! ILLEGAL TARGET NUMBER!!!!\n"},"First missing target stops the scan");
        f.targets({6,-1});f.entities[6].actor=&f.actors[6].actor;
        rejects_with([&]{okage::battle_party_first_update(s,f.root,f.entities);},"outside native entity bindings");
        require(s.first_callback==Callback::Roster,"Unsupported negative target never installs");
    }
    {
        SequenceFixture f;auto& s=f.stan();f.targets({});put(s.storage,0x21c,8);put(s.storage,0x24,19);
        s.model->animation().set_channel(1,{9,9,9,1});
        okage::battle_party_first_update(s,f.root,f.entities);
        require(s.first_callback==Callback::ActionSequence&&s.saved_first_callback==Callback::Roster&&
                word(s.storage,0x24)==0&&word(s.storage,0x30)==3&&word(s.storage,0)==0&&word(s.storage,0x28)==0,
                "State8 installs 156c70, saves the roster callback and records the selector");
        for(unsigned i=0;i<4;++i)require(s.model->animation().value(1)[i]==number(s.storage,0x60+4*i),"Install path keeps the channel tail");
        require(word(s.storage,0x21c)==8,"154b38 leaves BODY state8 for the callback");
    }
}
void action_sequence_paths() {
    {
        SequenceFixture f;f.installed();auto& s=f.stan();put(f.root.bytes,0x81a8,0);
        const auto before=s.storage;const auto root=f.root.bytes;
        okage::battle_actor_first_update(s,f.root,f.entities);
        require(s.storage==before&&f.root.bytes==root,"Closed 151978 gate returns before any write");
        put(s.storage,0x24,4);okage::battle_actor_first_update(s,f.root,f.entities);
        require(word(s.storage,0x24)==4&&s.first_callback==Callback::ActionSequence,"Unlisted substeps return without work");
    }
    {   //BODY+1d8==0x1c: duration -1 without a secondary owner; substep1 does
        //not reselect, then increments twice when the deadline already passed.
        SequenceFixture f;f.installed();auto& s=f.stan();
        put(s.storage,0x218,0x1c,2);put(s.storage,0x21a,6,2);put(s.storage,0x1fc,0x1c,2);
        s.action_motion.emplace();number(s.action_motion->bytes,0,0);
        std::vector<int> names;okage::BattleStrikeServices services;
        services.action_name=[&](std::int16_t action){names.push_back(action);};
        okage::battle_actor_first_update(s,f.root,f.entities,services);
        require(s.model->motion()==6&&word(s.storage,0x128)==6&&word(s.storage,0x12c)==0xbf800000u&&
                names==std::vector<int>{0x1c}&&word(s.storage,0x24)==1,"Selector0x1c uses -1 and the BODY+1da motion");
        okage::battle_actor_first_update(s,f.root,f.entities,services);
        require(s.model->motion()==6&&word(s.storage,0x12c)==0xbf800000u&&word(s.storage,0x24)==3&&
                number(s.storage,0x1f8)==1&&word(s.storage,0x300)==0,"Selector0x1c skips the idle reselect; passed deadline advances twice");
    }
    {
        SequenceFixture f;f.installed();auto& s=f.stan();put(s.storage,0x24,2);
        number(s.storage,0x2c,.1f);number(s.storage,0x1f8,1);
        s.action_motion.emplace();put(s.action_motion->bytes,0,0x3f8ccccc);
        for(unsigned at:{0x1e4u,0x1f4u})put(s.storage,at,4);put(s.storage,0x1ec,8);
        okage::battle_actor_first_update(s,f.root,f.entities);
        require(word(s.storage,0x1f8)==0x3f8ccccc&&word(s.storage,0x24)==2,"Toward-zero sum equals the deadline, which is not expiry");
        okage::battle_actor_first_update(s,f.root,f.entities);
        require(word(s.storage,0x24)==3,"The following strictly greater sum expires");
    }
    for(int selector:{0x71,0xc0,0x72,0xc1,205}) {
        //Substep3: 151170(1), then only 0x71/0xc0 (+50) and 0x72/0xc1 (+100)
        //restore the live HP word clamped at TASK164 and spawn1015e8/1016a8.
        SequenceFixture f;f.installed();auto& s=f.stan();put(s.storage,0x24,3);
        put(s.storage,0x1fc,std::uint32_t(selector),2);put(s.storage,0x164,120,2);put(f.actors[3].record,8,40);
        f.targets({6,8});put(s.storage,0x142,205,2);put(s.storage,0x144,10,2);put(s.storage,0x146,0,2);
        s.storage[0x13c]=0x5a;put(s.storage,0x34,9);
        std::vector<std::string> events;okage::BattleStrikeServices services;
        okage::BattleHitServices hits;
        hits.attached_effect=[&](int id,okage::BattleActor& owner){require(&owner==&s,"HP effect attaches to BODY+e0");events.push_back("attach "+std::to_string(id));return 44u;};
        hits.effect_value=[&](unsigned handle,int value){events.push_back("value "+std::to_string(handle)+" "+std::to_string(value));};
        rejects_with([&]{okage::battle_actor_first_update(s,f.root,f.entities,services,&hits);},"camera151170");
        require(word(s.storage,0x21c)==0&&word(s.storage,0x24)==3,"Missing camera owner stops before the tail");
        services.camera_sequence=[&](int row){events.push_back("camera "+std::to_string(row));};
        okage::battle_actor_first_update(s,f.root,f.entities,services,&hits);
        const int amount=selector==0x71||selector==0xc0?50:selector==205?0:100;
        std::vector<std::string> expected{"camera 1"};
        if(amount){expected.push_back("attach 35");expected.push_back("value 44 "+std::to_string(-amount));}
        require(events==expected,"Camera precedes the selector-only restore effect");
        require(word(f.actors[3].record,8)==(amount==50?90u:amount==100?120u:40u),"ADDIU then signed clamp to TASK164");
        require(word(s.storage,0x21c)==99&&s.storage[0x132]==0xff&&s.storage[0x13b]==0xff&&s.storage[0x13c]==0x5a&&
                half(s.storage,0x140)==0&&half(s.storage,0x142)==0&&half(s.storage,0x144)==0&&half(s.storage,0x146)==0,
                "State99 and 1544b0 clear exactly ten targets and four halfwords");
        require(s.first_callback==Callback::Roster&&word(s.storage,0x24)==0&&word(s.storage,0x34)==0,
                "Tail restores the saved callback and resets substep and param13");
    }
    {
        SequenceFixture f;f.installed();auto& s=f.stan();put(s.storage,0x21a,7,2);
        s.action_secondary.emplace();number(s.action_secondary->bytes,0,160);
        rejects_with([&]{okage::battle_actor_first_update(s,f.root,f.entities);},"HUD168380");
        require(okage::battle_command_available(f.root)==0&&word(f.root.bytes,0x81a4)==1&&word(s.storage,0x24)==0,
                "Missing 168380 owner retains the preceding command push");
    }
}
std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("Cannot read owned stage input "+path.string());
    return {std::istreambuf_iterator<char>(in),{}};
}
void action205_owned(const std::filesystem::path& stage) {
    okage::BattleActionData data(read_file(stage/"NATIVE_UI/battle-actions.bin"));
    okage::GameDataTables tables(read_file(stage/"CMNDATA/XDB/BKDATA.XDB"),read_file(stage/"CMNDATA/XDB/ITEM.XDB"));
    okage::RuwResource ruw(read_file(stage/"NATIVE_UI/BTL.RUW"));
    okage::BattleHud hud;
    SequenceFixture f;auto& s=f.stan();
    put(s.storage,0x142,205,2);put(s.storage,0x144,10,2);put(s.storage,0x146,0,2);f.targets({});
    (void)okage::battle_setup_action(s,f.root,tables,data);
    require(s.action_motion->identity==318&&s.action_secondary->identity==360,
            "Owned action205 resolves motion318 and secondary360");
    require(s.storage[0x132]==6&&s.storage[0x133]==8&&s.storage[0x134]==0xff&&half(s.storage,0x140)==2&&
            half(s.storage,0x1fc)==205&&half(s.storage,0x218)==205&&half(s.storage,0x21a)==7,
            "Owned registration matches the natural row targets 06 08 ff");
    number(s.storage,0x2c,.5f);put(s.storage,0x21c,8);
    okage::battle_party_first_update(s,f.root,f.entities);
    require(s.first_callback==Callback::ActionSequence&&s.saved_first_callback==Callback::Roster,"Owned state8 installs 156c70");
    std::vector<std::string> events;
    okage::BattleStrikeServices services;
    services.damage_event=[&](int command,int owner,int target){events.push_back("damage "+std::to_string(command)+" "+std::to_string(owner)+" "+std::to_string(target));};
    services.sound=[&](int id){events.push_back("sound "+std::to_string(id));};
    services.positioned_effect=[&](int id,const okage::Float4&,bool){events.push_back("effect "+std::to_string(id));return 70u+unsigned(events.size());};
    services.action_name=[&](std::int16_t action){events.push_back("hud "+std::to_string(action));hud.show_action_name(ruw,tables,action);};
    services.camera_sequence=[&](int row){events.push_back("camera "+std::to_string(row));};
    put(f.root.bytes,0x81a8,0);
    okage::battle_actor_first_update(s,f.root,f.entities,services);
    require(events.empty()&&word(s.storage,0x24)==0,"Substep0 waits for the 151978 command gate");
    put(f.root.bytes,0x81a8,1);const auto queue=word(f.root.bytes,0x81a4);
    okage::battle_actor_first_update(s,f.root,f.entities,services);
    require(s.model->motion()==7&&word(s.storage,0x128)==7&&number(s.storage,0x12c)==160&&word(s.storage,0x20)==0,
            "Substep0 1546a0 uses secondary type7 and its authored 160 duration");
    const unsigned slot=0x80a4+(queue+1)*8;
    require(word(f.root.bytes,0x81a4)==queue+1&&word(f.root.bytes,slot)==3&&word(f.root.bytes,slot+4)==1&&
            okage::battle_command_available(f.root)==0,"1517e0(owner3,1) closes the command gate");
    require(events==std::vector<std::string>{"hud 205"}&&word(s.storage,0x300)==0&&word(s.storage,0x308)==0xffffffffu&&
            word(s.storage,0x24)==1,"Substep0 HUD, BODY+2c0/2c8 writes and advance");
    const auto& text=*ruw.widget(5,0x2e).runtime->text;
    require(std::string(text.data())=="\\f[2]\\s[0.95,1.0]Burning Devil"&&
            ruw.widget(5,0x2e).runtime->words[6]==std::uint32_t(320-5*13)&&hud.type3_reset==1&&hud.type3_ready==1,
            "168380 formats sysmes0x72 with the Waza name, centers by strlen and sets both HUD flags");
    for(const auto& w:ruw.widgets()[5])if(w.runtime&&w.words[8]==0xffffffffu)
        require((w.runtime->words[4]>>24)==0,"168380 hides class-1 rows");
    okage::battle_actor_first_update(s,f.root,f.entities,services);
    require(s.model->motion()==1&&number(s.storage,0x12c)==-1&&number(s.storage,0x1f8)==.5f&&word(s.storage,0x24)==2,
            "Substep1 reselects idle, then accumulates the first half step");
    require(events==std::vector<std::string>{"hud 205","damage 16 3 6","damage 16 3 8","sound 54","effect 12","effect 12"},
            "154f98 damage per live target, then 1550d0 sound, then 155180 type2 effects");
    require(word(s.storage,0x1e4)==4&&word(s.storage,0x1ec)==8&&word(s.storage,0x1f4)==4,"Authored -1 next times terminate each schedule");
    okage::battle_actor_first_update(s,f.root,f.entities,services);
    require(number(s.storage,0x1f8)==1&&word(s.storage,0x24)==2&&events.size()==6,"Equality with the 1.0 deadline stays in substep2");
    put(s.storage,0x300,5);
    okage::battle_actor_first_update(s,f.root,f.entities,services);
    require(number(s.storage,0x1f8)==1.5f&&word(s.storage,0x24)==3&&word(s.storage,0x300)==0&&events.size()==6,
            "Passing the deadline advances and clears BODY+2c0 without schedule calls");
    okage::battle_actor_first_update(s,f.root,f.entities,services);
    require(events.back()=="camera 1"&&events.size()==7&&word(s.storage,0x21c)==99&&s.storage[0x132]==0xff&&
            half(s.storage,0x140)==0&&half(s.storage,0x142)==0&&s.first_callback==Callback::Roster&&word(s.storage,0x24)==0,
            "Substep3 camera, state99, participant clear and callback restoration");
    okage::battle_actor_first_update(s,f.root,f.entities,services);
    require(word(s.storage,0x21c)==99&&events.size()==7,"Restored roster callback runs state99 without new services");
    std::cout<<"Owned action205 state8/156c70 sequence passed\n";
}
void stan_preemptive_owned(const std::filesystem::path& stage) {
    using Callback=okage::BattleActor::FirstCallback;
    okage::BattleActionData data(read_file(stage/"NATIVE_UI/battle-actions.bin"));
    okage::GameDataTables tables(read_file(stage/"CMNDATA/XDB/BKDATA.XDB"),read_file(stage/"CMNDATA/XDB/ITEM.XDB"));
    okage::RuwResource ruw(read_file(stage/"NATIVE_UI/BTL.RUW"));
    okage::BattleHud hud;
    const auto& waza=tables.action(0xcf);
    require(waza.name=="Raging Devil"&&waza.words[10]==2&&waza.words[11]==0,"Owned waza0xcf is type2 with delay0");
    SequenceFixture f;auto& s=f.stan();
    for(unsigned i=0;i<14;++i)put(f.actors[i].actor.storage,0x46,i<3?11:i==3?20:i==5||i==6?0:1,2);
    std::vector<okage::BattleActor*> party;
    for(unsigned i=0;i<3;++i) {
        auto& a=f.actors[i].actor;a.stat_bindings[2]=std::span(f.actors[i].record).subspan(0x4a,2);
        put(a.storage,0x21c,1);
        party.push_back(&a);
    }
    //status_block: ten 0xff target bytes and zero count/action/group/target.
    std::fill_n(s.storage.begin()+0x132,10,0xff);
    for(unsigned at:{0x140u,0x142u,0x144u,0x146u})put(s.storage,at,0,2);
    put(s.storage,0x21c,1);number(s.storage,0x194,3);number(s.storage,0x19c,7);number(s.storage,0x1a0,9);
    put(s.storage,0x304,4);
    okage::BattleEntryContext entry;entry.reset(1,0,0,0x1234); //native_runtime phase13 order.
    const auto entry_before=entry.bytes();
    std::array<std::uint8_t,0x1000> variables{};
    unsigned draws=0,sounds=0;
    okage::BattleRosterUpdateContext c{f.root,entry,variables,f.entities,0,0x40,[&](auto){++sounds;},
        [&]{++draws;return 0x1234u;},&tables,&data,nullptr};
    okage::battle_party_roster_update(party,s,c);
    require(entry.parameters().argument1==0,"152dc0 clears the entry parameter halfword");
    for(unsigned i=0;i<entry_before.size();++i)
        require(entry.bytes()[i]==((i==0x30||i==0x31)?0:entry_before[i]),"Only halfword+30 is consumed");
    require(half(s.storage,0x142)==0xcf&&half(s.storage,0x144)==10&&half(s.storage,0x146)==0,"Waza0xcf, group10, target0");
    require(s.action_record_id==0xcf&&s.action_motion->identity==320&&s.action_secondary->identity==361&&
            half(s.storage,0x1fc)==0xcf&&half(s.storage,0x218)==0xcf&&half(s.storage,0x21a)==7,
            "1580d0 resolves owned motion320 and mode0 secondary361 type7");
    require(half(s.storage,0x140)==2&&s.storage[0x132]==6&&s.storage[0x133]==8&&s.storage[0x134]==0xff,
            "Targets are the live enemy range after the second registration");
    const auto owner=f.root.entity(3);
    require(half(owner,0x60)==3&&half(owner,0x62)==0xffff&&half(owner,0x64)==2&&word(owner,0x68)==0&&half(owner,0x16c)==2,
            "Two 158548 calls: two owner registrations and self links, flag0");
    require(f.root.action_links[3][0]==3&&f.root.action_links[3][1]==3&&!f.root.action_links[3][2],"Owner self links");
    require(f.root.action_links[6][0]==3&&f.root.action_links[6][1]==3&&half(f.root.entity(6),0x16c)==2,
            "Stale group0 pass and group10 pass both link enemy6");
    require(f.root.action_links[8][0]==3&&!f.root.action_links[8][1]&&half(f.root.entity(8),0x16c)==1,
            "Group1 enemy8 is linked only by the group10 pass");
    for(unsigned i:{0u,1u,2u,4u,5u,7u,9u,13u})require(!f.root.action_links[i][0],"No other entity is linked");
    require(word(s.storage,0x1a0)==0&&word(s.storage,0x21c)==3&&word(s.storage,0x19c)==0&&number(s.storage,0x194)==3,
            "Returned 0*5 delay, state3, wait timer0; charge timer preserved");
    require(draws==0&&sounds==0,"Pre-emptive setup consumes no RNG and no ready sound");
    okage::battle_party_roster_update(party,s,c);
    require(word(s.storage,0x21c)==8&&word(s.storage,0x194)==0&&word(s.storage,0x304)==5,
            "1588b0 readiness1 selects state8 (only 0xce selects9) and counts the action");
    okage::battle_party_roster_update(party,s,c);
    require(word(s.storage,0x21c)==8&&s.first_callback==Callback::Roster,"Roster state8 waits for the actor callback");
    number(s.storage,0x2c,.5f);
    okage::battle_party_first_update(s,f.root,f.entities);
    require(s.first_callback==Callback::ActionSequence&&s.saved_first_callback==Callback::Roster&&word(s.storage,0x30)==3,
            "154b38 state8 installs 156c70 with the sign-extended owner in TASK+30");
    std::vector<std::string> events;
    okage::BattleStrikeServices services;
    services.damage_event=[&](int command,int owner,int target){events.push_back("damage "+std::to_string(command)+" "+std::to_string(owner)+" "+std::to_string(target));};
    services.sound=[&](int id){events.push_back("sound "+std::to_string(id));};
    services.positioned_effect=[&](int id,const okage::Float4&,bool){events.push_back("effect "+std::to_string(id));return 70u+unsigned(events.size());};
    services.action_name=[&](std::int16_t action){events.push_back("hud "+std::to_string(action));hud.show_action_name(ruw,tables,action);};
    services.camera_sequence=[&](int row){events.push_back("camera "+std::to_string(row));};
    const auto queue=word(f.root.bytes,0x81a4);
    std::vector<unsigned> substeps;std::vector<float> clocks;
    for(unsigned tick=0;tick<64&&s.first_callback==Callback::ActionSequence;++tick) {
        okage::battle_actor_first_update(s,f.root,f.entities,services);
        substeps.push_back(word(s.storage,0x24));clocks.push_back(number(s.storage,0x1f8));
        if(tick==0)require(s.model->motion()==7&&number(s.storage,0x12c)==160,"Substep0 selects secondary361 type7 for 160");
    }
    require(substeps==std::vector<unsigned>{1,2,2,3,0}&&clocks==std::vector<float>{0,.5f,1,1.5f,1.5f},
            "156c70 substeps and toward-zero clock at dt0.5");
    require(events==std::vector<std::string>{"hud 207","damage 16 3 6","damage 16 3 8","sound 62","effect 14","effect 14","camera 1"},
            "HUD, per-live-target damage, sound, type2 effects, then camera row1");
    require(s.first_callback==Callback::Roster&&word(s.storage,0x24)==0&&word(s.storage,0x34)==0,"156c70 restores the roster callback");
    require(word(s.storage,0x21c)==99&&half(s.storage,0x140)==0&&half(s.storage,0x142)==0&&half(s.storage,0x144)==0&&
            half(s.storage,0x146)==0&&s.storage[0x132]==0xff&&s.storage[0x13b]==0xff,"1544b0 clears the action and targets");
    require(word(f.root.bytes,0x81a4)==queue+1,"1517e0 pushed one command");
    const auto& text=*ruw.widget(5,0x2e).runtime->text;
    require(std::string(text.data())=="\\f[2]\\s[0.95,1.0]Raging Devil"&&ruw.widget(5,0x2e).runtime->words[6]==std::uint32_t(320-5*12),
            "168380 shows the owned Waza name");
    okage::battle_party_roster_update(party,s,c);
    require(word(s.storage,0x21c)==1&&word(s.storage,0x194)==0&&word(s.storage,0x19c)==0&&
            half(f.root.entity(3),0x60)==0xffff&&half(f.root.entity(3),0x62)==0xffff&&word(f.root.entity(3),0x68)==0,
            "152dc0 state99: state1, 158868 invalidation, both timers cleared");
    const auto settled=s.storage;
    okage::battle_party_roster_update(party,s,c);
    require(s.storage==settled&&entry.parameters().argument1==0&&draws==0&&sounds==0,
            "Cleared parameter returns Stan to the ordinary HERO-trigger scan, which waits");
    std::cout<<"Owned Stan pre-emptive 0xcf sequence passed\n";
}
}
int main(int argc,char** argv){try {
    first_callback();charge_publication();ready_actor_keeps_charge();action_wait_callbacks();attack_dispatch_and_approach();ballistic_attack();ranged_attack();ballistic_cartesian_equivalence();strike_schedules();channels_and_effect_guards();enemy_paths();floating_environment();victory_status_reset();victory_actor_states();queued_party_hit();party_death_lifecycle();queued_enemy_hit();
    state8_install_guards();action_sequence_paths();
    if(argc==2) {
        const std::filesystem::path stage=argv[1];
        if(std::filesystem::exists(stage/"NATIVE_UI/battle-actions.bin")){action205_owned(stage);stan_preemptive_owned(stage);}
        else std::cout<<"Owned action205 section skipped: stage lacks NATIVE_UI/battle-actions.bin\n";
    } else require(argc==1,"Expected optional owned stage directory");
    std::cout<<"Battle actor startup callback tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
