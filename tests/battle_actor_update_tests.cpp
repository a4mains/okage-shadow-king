#include "okage/battle_actor.hpp"
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
void number(std::span<std::uint8_t> b,unsigned at,float value){put(b,at,__builtin_bit_cast(std::uint32_t, (value)));}
float number(okage::Bytes b,unsigned at){return __builtin_bit_cast(float, (word(b,at)));}
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
        std::vector<std::uint8_t> motion(56);
        put(motion,0,0x544d58);put(motion,4,24);put(motion,16,0xffffffff);put(motion,24,1);
        put(motion,32,48);put(motion,48,0x00a00002);put(motion,52,0);
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
    number(f.actor.storage,0x12c,-1);put(f.actor.storage,0x21c,99);
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
}
int main(){try {
    first_callback();charge_publication();channels_and_effect_guards();enemy_paths();floating_environment();
    std::cout<<"Battle actor startup callback tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
