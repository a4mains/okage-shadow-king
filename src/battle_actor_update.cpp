#include "okage/battle_actor.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
void check(bool valid,const char* message) {if(!valid)throw std::runtime_error(message);}
std::uint32_t word(Bytes b,unsigned at) {
    check(at<=b.size()&&b.size()-at>=4,"Battle actor update word extent");
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|
           (std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
std::int16_t half(Bytes b,unsigned at) {
    check(at<=b.size()&&b.size()-at>=2,"Battle actor update halfword extent");
    return __builtin_bit_cast(std::int16_t, (std::uint16_t(b[at]|(unsigned(b[at+1])<<8))));
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t value,unsigned size=4) {
    check(at<=b.size()&&b.size()-at>=size,"Battle actor update write extent");
    for(unsigned i=0;i<size;++i)b[at+i]=std::uint8_t(value>>(8*i));
}
float supported(float value) {
    const auto bits=__builtin_bit_cast(std::uint32_t, (value)),exponent=bits&0x7f800000u;
    check(exponent!=0x7f800000u&&(exponent!=0||(bits&0x7fffffu)==0),
          "Battle actor update nonfinite/subnormal arithmetic is unsupported");
    return value;
}
float scalar(Bytes b,unsigned at) {return supported(__builtin_bit_cast(float, (word(b,at))));}
void scalar(std::span<std::uint8_t> b,unsigned at,float value) {put(b,at,__builtin_bit_cast(std::uint32_t, (value)));}
class TowardZero {
public:
    TowardZero() {
        check(std::fegetenv(&saved_)==0,"Cannot save battle actor floating environment");
        if(std::fesetround(FE_TOWARDZERO)!=0) {
            std::fesetenv(&saved_);throw std::runtime_error("Cannot set battle actor toward-zero rounding");
        }
    }
    ~TowardZero(){std::fesetenv(&saved_);}
    TowardZero(const TowardZero&)=delete;
    TowardZero& operator=(const TowardZero&)=delete;
private:
    std::fenv_t saved_{};
};
enum class Operation {Add,Subtract,Multiply,Divide};
float single(float left,float right,Operation operation) {
    supported(left);supported(right);
    if(operation==Operation::Divide)check(right!=0,"Battle actor zero divisor is unsupported");
    check(std::feclearexcept(FE_OVERFLOW|FE_UNDERFLOW)==0,"Cannot clear battle actor arithmetic flags");
    volatile float a=left,b=right,result;
    switch(operation) {
    case Operation::Add:result=a+b;break;
    case Operation::Subtract:result=a-b;break;
    case Operation::Multiply:result=a*b;break;
    case Operation::Divide:result=a/b;break;
    }
    check(!(std::fetestexcept(FE_OVERFLOW|FE_UNDERFLOW)),
          "Battle actor floating overflow/underflow is unsupported");
    return supported(result);
}
Float4 vector(Bytes b,unsigned at) {
    Float4 value{};for(unsigned i=0;i<4;++i)value[i]=__builtin_bit_cast(float, (word(b,at+i*4)));
    return value;
}
std::int8_t status(const BattleActor& actor,unsigned binding) {
    check(!actor.stat_bindings[binding].empty(),"Battle actor live status binding is missing");
    return __builtin_bit_cast(std::int8_t, (actor.stat_bindings[binding][0]));
}
FieldActor& model(BattleActor& actor) {
    check(bool(actor.model),"Battle actor model owner is missing");return *actor.model;
}
void step_model(BattleActor& actor) {
    //1547b8 takes B=C+40. B+d8 points to C+2c; B+dc is C+11c.
    const float timestep=scalar(actor.storage,0x2c);
    if(timestep>1.0f) {
        put(actor.storage,0x11c,0);
        auto& instance=model(actor);
        //1bab08(model,2): visibility gates BOTH channel advancement and bones.
        for(unsigned i=0;i<2;++i)if(instance.visible) {
            instance.tick_channels();
            if(instance.has_motion())instance.evaluate_bones(); // Original flag200.
        }
    } else {
        const float accumulated=single(scalar(actor.storage,0x11c),timestep,Operation::Add);
        scalar(actor.storage,0x11c,accumulated);
        if(accumulated>=1.0f) {
            put(actor.storage,0x11c,0);
            model(actor).tick_channels();
        }
    }
}
void initial_status_and_motion(BattleActor& actor,const BattleState& root) {
    const auto index=half(actor.storage,0x130);
    check(index>=0&&unsigned(index)<battle_entity_count,"Battle actor entity index outside native root");
    check(half(root.bytes,0x1d2+unsigned(index)*0x200)==0,
          "Unrecovered battle actor queued status/action");
    const float remaining=scalar(actor.storage,0x12c);
    if(remaining>=0.0f) {
        const float next=single(remaining,scalar(actor.storage,0x2c),Operation::Subtract);
        scalar(actor.storage,0x12c,next);
        check(next>=0.0f,"Unrecovered battle actor motion expiry/effect cleanup");
    }
}
void primary_effect(BattleActor& actor) {
    //1548e0: status1 or outside1..5 has no work unless its effect is active.
    const auto primary=status(actor,1);
    check(!(primary>1&&primary<6)&&word(actor.storage,0x310)!=1,
          "Unrecovered battle actor primary status effect");
}
}
void battle_actor_motion_update(BattleActor& actor) {
    TowardZero rounding;
    put(actor.storage,0x20,0);
    switch(word(actor.storage,0x21c)) { //B+1dc, not C+1dc.
    case 0:case 2:
        break;
    case 1: {
        const float step=single(scalar(actor.storage,0x2c),scalar(actor.storage,0x1a4),Operation::Multiply);
        const float next=single(scalar(actor.storage,0x194),step,Operation::Add);
        const float maximum=scalar(actor.storage,0x198);
        scalar(actor.storage,0x194,next);
        if(maximum<=next)scalar(actor.storage,0x194,maximum);
        break;
    }
    default:throw std::runtime_error("Unrecovered battle actor motion/action state");
    }
    auto& animation=model(actor).animation();
    animation.set_channel(0,vector(actor.storage,0x90));
    animation.set_channel(1,vector(actor.storage,0x60));
    animation.set_channel(2,vector(actor.storage,0xc0));
}
void battle_party_first_update(BattleActor& actor,const BattleState& root) {
    check(!actor.enemy&&!actor.dead_callback,"Unrecovered party first callback identity");
    TowardZero rounding;
    if(half(actor.storage,0x42)!=3)initial_status_and_motion(actor,root);
    battle_actor_motion_update(actor);
}
void battle_actor_second_suffix(BattleActor& actor) {
    TowardZero rounding;
    if(word(actor.storage,0x20)==0)step_model(actor);
    primary_effect(actor);
    //154a38 has a different status pointer and a separate effect owner.
    const auto secondary=status(actor,3);
    check(!(secondary>0&&secondary<10)&&word(actor.storage,0x320)!=1,
          "Unrecovered battle actor secondary status effect");
    check(word(actor.storage,0x17c)!=1&&word(actor.storage,0x330)!=1,
          "Unrecovered battle actor auxiliary status effect");
}
void battle_party_second_update(BattleActor& actor,const BattleState& root) {
    check(!actor.enemy,"Party second callback received enemy actor");
    TowardZero rounding;
    const float ratio=single(scalar(actor.storage,0x194),scalar(actor.storage,0x198),Operation::Divide);
    const float percent=single(ratio,100.0f,Operation::Multiply);
    check(double(percent)>=double(std::numeric_limits<std::int32_t>::min())&&
          double(percent)<=double(std::numeric_limits<std::int32_t>::max()),
          "Battle party charge conversion outside signed32 scope");
    const auto charge=static_cast<std::int32_t>(percent); //CVT.W.S, toward zero.
    check(word(root.bytes,0x81b4)!=3,"Unrecovered battle party transition to callback153dc0");
    // No clamp: only preserve an already-full charge if the new value is full.
    if(half(actor.record,0x26)<100||charge<100)put(actor.record,0x26,std::uint32_t(charge),2);
    battle_actor_second_suffix(actor);
}
void battle_enemy_first_update(BattleActor& actor,const BattleState& root) {
    check(actor.enemy&&!actor.dead_callback,"Unrecovered enemy first callback identity");
    TowardZero rounding;
    put(actor.storage,0x20,0);
    initial_status_and_motion(actor,root);
    battle_actor_motion_update(actor); //154b38 writes C+20 a second time.
}
void battle_enemy_second_update(BattleActor& actor,const BattleState& root) {
    check(actor.enemy,"Enemy second callback received party actor");
    TowardZero rounding;
    check(word(root.bytes,0x81b4)!=4,"Unrecovered battle enemy transition to callback15c1a8");
    if(word(actor.storage,0x20)==0)step_model(actor);
    primary_effect(actor);
}
}
