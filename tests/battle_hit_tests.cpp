#include "okage/battle_hit.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>
#include <source_location>
#include <stdexcept>
#include <string>

namespace {
void require(bool value,const char* message,const std::source_location at=std::source_location::current()) {
    if(!value)throw std::runtime_error(std::string(message)+" at line "+std::to_string(at.line()));
}
template<class F> void rejects(F action) {
    try{action();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected hit scope/service rejection");
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)b[at+i]=std::uint8_t(value>>(8*i));
}
std::uint32_t word(okage::Bytes b,unsigned at=0) {
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
void number(std::span<std::uint8_t> b,unsigned at,float v){put(b,at,std::bit_cast<std::uint32_t>(v));}
struct Fixture {
    okage::BattleActor actor;
    std::array<std::uint8_t,4> hp{},replacement_hp{};
    std::array<std::uint8_t,1> primary{1};
    okage::BattleHitServices services;
    std::vector<std::string> calls;
    int fail=0,number_value=0,history_delta=0,history_category=-1,impact_id=0;
    okage::Float4 point{};
    bool source_w=true;
    Fixture() {
        actor.stat_bindings[0]=hp;actor.stat_bindings[1]=primary;put(hp,0,100);
        number(actor.storage,0x4c,5);number(actor.storage,0x12c,-1);
        std::vector<std::uint8_t> motion(104);
        put(motion,0,0x544d58);put(motion,4,72);put(motion,16,0xffffffff);put(motion,24,1);
        put(motion,32,48);put(motion,48,0x00a00002);put(motion,52,0);
        okage::ModelFile model;model.models.emplace_back().bones.resize(1);
        actor.model=std::make_unique<okage::FieldActor>(std::move(model),motion,okage::TextureAnimation{},true,true);
        actor.model->animation().set_channel(1,{11,20,33,7});
        services.attached_effect=[&](int id,okage::BattleActor& a) {
            require(&a==&actor,"Attached owner");event("attached"+std::to_string(id));return 17u;
        };
        services.effect_value=[&](unsigned handle,int value) {
            require(handle==17,"Number handle");number_value=value;event("number");
        };
        services.history=[&](okage::BattleActor& a,std::int16_t delta,std::int16_t category) {
            require(&a==&actor,"History owner");history_delta=delta;history_category=category;event("history");
        };
        services.death_history=[&](okage::BattleActor& a) {
            require(&a==&actor&&word(a.stat_bindings[0])==0,"Death history after HP zero");event("death");
        };
        services.positioned_effect=[&](int id,const okage::Float4& p,bool exact_w) {
            impact_id=id;point=p;source_w=exact_w;event("impact");return 99u;
        };
    }
    void event(std::string name) {
        calls.push_back(std::move(name));if(fail==int(calls.size()))throw std::runtime_error("Injected service failure");
    }
    int hit(std::int16_t category=0,std::int16_t amount=20) {
        return okage::battle_apply_hit(actor,category,amount,services);
    }
};
void miss_and_guards() {
    for(int amount:{0,-1,-32768}) {
        Fixture f;const auto before=f.actor.storage;f.actor.model.reset();f.actor.stat_bindings[0]={};
        require(f.hit(0,std::int16_t(amount))==0,"Nonpositive result");
        require(f.calls==std::vector<std::string>{"attached36"}&&f.actor.storage==before,"Miss only creates effect36");
    }
    for(int category:{2,3,4,5,13,14,16,35,55,57,58}) {
        Fixture f;rejects([&]{f.hit(std::int16_t(category));});
        require(f.calls.empty()&&word(f.hp)==100,"Unrecovered category guards before effects");
    }
    //159d00 jump-table defaults: no service, HP, motion or storage change.
    for(int category:{-32768,-1,6,7,8,9,12,59,999,32767}) {
        Fixture f;f.services={};const auto before=f.actor.storage;
        require(f.hit(std::int16_t(category))==0&&f.actor.storage==before&&word(f.hp)==100&&f.primary[0]==1,
                "Jump-table default category returns0 without effects");
    }
    for(int category:{0,1,10,11,0x38,-1,999}) {
        Fixture f;f.primary[0]=6;f.services={};const auto before=f.actor.storage;
        require(f.hit(std::int16_t(category))==0&&f.actor.storage==before&&word(f.hp)==100,
                "Primary6 target ignores every non-revival category");
    }
    for(int category:{6,7}) {
        Fixture f;f.primary[0]=6;rejects([&]{f.hit(std::int16_t(category));});
        require(f.calls.empty()&&word(f.hp)==100,"Primary6 revival categories6/7 guard first");
    }
}
struct Special : Fixture {
    std::int16_t flag=0;std::uint32_t available=1;
    std::vector<std::pair<std::int32_t,std::int32_t>> pushes;std::vector<std::int32_t> spent;
    Special() {
        services.entry_escape_flag=[&]{event("14ad10");return flag;};
        services.command_available=[&]{event("151978");return available;};
        services.command_push=[&](std::int32_t owner,std::int32_t command){event("1517e0");pushes.push_back({owner,command});};
        services.spend_money=[&](std::int32_t value){event("15abf8");spent.push_back(value);};
    }
};
void escape_categories() {
    for(int amount:{0,1,-1,999}) {
        Special f;const auto before=f.actor.storage;
        require(f.hit(10,std::int16_t(amount))==0&&f.pushes==std::vector<std::pair<std::int32_t,std::int32_t>>{{0,3}},
                "Category10 pushes (owner0, command3) regardless of amount");
        require(f.calls==std::vector<std::string>{"14ad10","151978","1517e0"}&&f.actor.storage==before&&word(f.hp)==100,
                "Category10 source order and no HP/motion effects");
    }
    const std::array<std::int32_t,5> money{0,100,1000,10000,100000};
    for(int amount:{-1,0,1,2,3,4,5}) {
        Special f;
        require(f.hit(11,std::int16_t(amount))==0&&f.pushes==std::vector<std::pair<std::int32_t,std::int32_t>>{{3,4}},
                "Category11 pushes (owner3, command4)");
        if(amount>=1&&amount<=4) {
            require(f.spent==std::vector<std::int32_t>{money[unsigned(amount)]}&&
                    f.calls==std::vector<std::string>{"14ad10","151978","1517e0","15abf8"},"Amounts1..4 spend after the push");
        } else require(f.spent.empty()&&f.calls.size()==3,"Other amounts take the second table's return0");
    }
    for(int category:{10,11}) {
        Special blocked;blocked.flag=1;
        require(blocked.hit(std::int16_t(category),2)==0&&blocked.calls==std::vector<std::string>{"14ad10"}&&blocked.pushes.empty(),
                "Nonzero bank7+36 returns before the command gate");
        Special negative;negative.flag=-1;
        require(negative.hit(std::int16_t(category),2)==0&&negative.pushes.empty(),"14ad10 tests the halfword for zero only");
        Special busy;busy.available=0;
        require(busy.hit(std::int16_t(category),2)==0&&busy.calls==std::vector<std::string>{"14ad10","151978"}&&
                busy.pushes.empty()&&busy.spent.empty(),"Closed command gate returns without push or spend");
        Special missing;missing.services.command_push={};
        rejects([&]{missing.hit(std::int16_t(category),2);});
        require(missing.calls==std::vector<std::string>{"14ad10","151978"},"Missing push service fails after both gates");
    }
    Special no_money;no_money.services.spend_money={};rejects([&]{no_money.hit(11,3);});
    require(no_money.pushes.size()==1,"Missing money service fails after the push");
}
void rage_category() {
    for(auto [start,expected]:std::array<std::pair<std::uint16_t,std::uint16_t>,6>{{{0,1},{3,4},{5,6},{6,6},{0x7fff,0x8000},{0xffff,0}}}) {
        Fixture f;f.services={};f.actor.storage[0x18c]=std::uint8_t(start);f.actor.storage[0x18d]=std::uint8_t(start>>8);
        auto before=f.actor.storage;before[0x18c]=std::uint8_t(expected);before[0x18d]=std::uint8_t(expected>>8);
        require(f.hit(0x38,1)==0&&f.actor.storage==before&&word(f.hp)==100&&f.primary[0]==1,
                "0x38 increments TASK18c with a signed>6 clamp and no other effect");
    }
}
void ordinary() {
    for(int category:{0,1})for(float size:{5.0f,10.0f,12.0f}) {
        Fixture f;number(f.actor.storage,0x4c,size);
        require(f.hit(std::int16_t(category))==10,"Survival result");
        require(word(f.hp)==80&&f.primary[0]==1,"Only HP changes");
        require(f.number_value==20&&f.history_delta==-20&&f.history_category==category,"Positive number and negative history");
        require(f.calls==std::vector<std::string>{"attached35","number","history","impact"},"Survivor service order");
        require(word(f.actor.storage,0x128)==4&&word(f.actor.storage,0x12c)==std::bit_cast<unsigned>(30.0f),"Motion4 duration30");
        require(f.actor.model->motion()==4,"Real model selected motion4");
        require(f.impact_id==(size==5?38:size==10?39:40),"Size selects impact");
        require(f.point==okage::Float4{11,20-size*.5f,33,7},"Impact uses live channel and preserves W");
        require(f.actor.model->animation().value(1)==okage::Float4{11,20,33,7},"Impact does not overwrite model position");
    }
    Fixture cartesian;cartesian.actor.cartesian_motion=true;cartesian.hit();
    require(!cartesian.source_w,"Impact propagates unavailable source W from Cartesian actor");
    for(unsigned hp:{0u,1u,20u,0xffffffffu}) {
        Fixture f;put(f.hp,0,hp);const auto before=f.actor.storage;
        require(f.hit()==6&&word(f.hp)==0,"Lethal clamps HP zero");
        require(f.calls==std::vector<std::string>{"attached35","number","history","death","impact"},"Death still impacts after history");
        require(f.actor.storage==before&&f.primary[0]==1,"Consumer leaves death callback/status to caller");
    }
    Fixture f;put(f.hp,0,0x80000000u);
    require(f.hit(0,1)==10&&word(f.hp)==0x7fffffffu,"HP subtraction wraps before signed lethal test");
    Fixture maximum;put(maximum.hp,0,40000);
    require(maximum.hit(1,32767)==10&&word(maximum.hp)==7233,"Full signed16 amount");
    require(maximum.number_value==32767&&maximum.history_delta==-32767,"Amount signs at services");
}
void failure_order() {
    for(int stage=1;stage<=4;++stage) {
        Fixture f;f.fail=stage;rejects([&]{f.hit();});
        require(int(f.calls.size())==stage,"Fail stops later services");
        require(word(f.hp)==(stage<=3?100u:80u),"HP stores only after history succeeds");
        require(word(f.actor.storage,0x128)==(stage==4?4u:0u),"Motion only after HP");
    }
    for(int stage:{4,5}) {
        Fixture f;put(f.hp,0,10);f.fail=stage;rejects([&]{f.hit();});
        require(word(f.hp)==0&&word(f.actor.storage,0x128)==0,"Lethal writes survive service failure without hit motion");
        require(int(f.calls.size())==stage,"Death service failure order");
    }
    Fixture missing_number;missing_number.services.effect_value={};rejects([&]{missing_number.hit();});
    require(missing_number.calls.size()==1&&word(missing_number.hp)==100,"Missing number service after attachment");
    Fixture missing_history;missing_history.services.history={};rejects([&]{missing_history.hit();});
    require(missing_history.calls.size()==2&&word(missing_history.hp)==100,"Missing history after number");
    Fixture missing_death;put(missing_death.hp,0,1);missing_death.services.death_history={};rejects([&]{missing_death.hit();});
    require(missing_death.calls.size()==3&&word(missing_death.hp)==0,"Missing death history after zero HP");
    Fixture missing_model;missing_model.actor.model.reset();rejects([&]{missing_model.hit();});
    require(word(missing_model.hp)==80&&word(missing_model.actor.storage,0x128)==4&&
            word(missing_model.actor.storage,0x12c)==std::bit_cast<unsigned>(-1.0f),"Motion selection prefix preserved on missing model");
    Fixture bad_binding;bad_binding.actor.stat_bindings[0]={};rejects([&]{bad_binding.hit();});
    require(bad_binding.calls.size()==3,"Missing HP binding checked only after history");
}
void live_services_and_rounding() {
    Fixture f;f.services.history=[&](okage::BattleActor& a,std::int16_t,std::int16_t) {
        f.event("history");put(f.replacement_hp,0,60);a.stat_bindings[0]=f.replacement_hp;
        number(a.storage,0x4c,10);a.model->animation().set_channel(1,{1,30,3,9});
    };
    require(f.hit()==10&&word(f.hp)==100&&word(f.replacement_hp)==40,"HP owner loaded after history");
    require(f.impact_id==39&&f.point==okage::Float4{1,25,3,9},"Impact reads live size/position after callbacks");
    Fixture rounded;number(rounded.actor.storage,0x4c,1);
    rounded.actor.model->animation().set_channel(1,{0,16777216.0f,0,1});
    const int saved=std::fegetround();std::fesetround(FE_UPWARD);
    require(rounded.hit()==10&&rounded.point[1]==16777215.0f,"Impact separate binary32 operations toward zero");
    require(std::fegetround()==FE_UPWARD,"Impact restores caller rounding");std::fesetround(saved);
    Fixture invalid;number(invalid.actor.storage,0x4c,std::numeric_limits<float>::infinity());
    rejects([&]{invalid.hit();});
    require(word(invalid.hp)==80&&word(invalid.actor.storage,0x128)==4,"Unsupported impact arithmetic preserves HP/motion prefix");
}
}
int main() {
    try {miss_and_guards();escape_categories();rage_category();ordinary();failure_order();live_services_and_rounding();std::cout<<"battle hit tests passed\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
