#include "okage/battle_actor.hpp"
#include "okage/battle_hit.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON

namespace {
void require(bool value,const char* message,unsigned at=__builtin_LINE()) {
    if(!value)throw std::runtime_error(std::string(message)+" at line "+std::to_string(at));
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
        std::vector<std::uint8_t> motion(104);
        put(motion,0,0x544d58);put(motion,4,72);put(motion,16,0xffffffff);put(motion,24,1);
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
    f.actor.first_callback=Callback::Roster;put(f.actor.storage,0x21c,5);put(f.actor.storage,0x24,13);put(target.actor.storage,0x48,1,2);
    rejects([&]{okage::battle_actor_first_update(f.actor,f.root,entities);});
    require(f.actor.first_callback==Callback::Roster&&word(f.actor.storage,0x24)==13,
            "Unsupported callback choice cannot silently install approach");
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
    std::vector<int> events;std::vector<okage::Float4> points;
    okage::BattleStrikeServices services;
    services.damage_event=[&](int command,int owner,int recipient){require(owner==0&&recipient==1,"Damage filters disabled target and uses signed owner");events.push_back(command);};
    services.sound=[&](int id){events.push_back(id);};
    services.positioned_effect=[&](int id,const okage::Float4& point){events.push_back(id);points.push_back(point);return unsigned(points.size()+10);};
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
    //Failure after sound retains elapsed, sound counter and previous effect handle.
    put(b,0x1ec,0);put(b,0x1f4,0);number(b,0x1e8,1);number(b,0x1f0,1);
    services.positioned_effect=[](int,const okage::Float4&)->unsigned{throw std::runtime_error("synthetic effect service failure");};
    rejects([&]{okage::battle_actor_first_update(a,f.root,entities,services);});
    require(number(b,0x1f8)==3&&word(b,0x1ec)==1&&word(b,0x1f4)==0&&word(b,0x30c)==12,
            "Service failure retains ordered prior stores without consuming effect entry");
    put(b,0x1ec,8);put(d,0x3c,3);
    services.positioned_effect=[](int,const okage::Float4&){return 27u;};
    rejects([&]{okage::battle_actor_first_update(a,f.root,entities,services);});
    require(word(b,0x30c)==27&&word(b,0x1f4)==0,"Unsupported scale retains preceding spawn and handle store");
    put(d,0x3c,4);put(b,0x140,0,2);
    okage::battle_actor_first_update(a,f.root,entities,services);
    require(word(b,0x1f4)==1&&word(b,0x30c)==27,"Scaled effect with no targets advances without invoking scale");
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
        [&](int id,const okage::Float4&){events.push_back(id);return 5u;}};
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
    okage::BattleStrikeServices sounds{[&](int id){events.push_back(id);},{},{}};
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
int main(){try {
    first_callback();charge_publication();ready_actor_keeps_charge();action_wait_callbacks();attack_dispatch_and_approach();strike_schedules();channels_and_effect_guards();enemy_paths();floating_environment();queued_enemy_hit();
    std::cout<<"Battle actor startup callback tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
