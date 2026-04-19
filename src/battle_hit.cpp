#include "okage/battle_hit.hpp"
#include <bit>
#include <array>
#include <cfenv>
#include <stdexcept>
#include <string>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::uint32_t word(Bytes b,unsigned at) {
    check(at<=b.size()&&b.size()-at>=4,"Battle hit word extent");
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|
           (std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t value) {
    check(at<=b.size()&&b.size()-at>=4,"Battle hit write extent");
    for(unsigned i=0;i<4;++i)b[at+i]=std::uint8_t(value>>(8*i));
}
float supported(float value) {
    const auto bits=std::bit_cast<std::uint32_t>(value),exponent=bits&0x7f800000u;
    check(exponent!=0x7f800000u&&(exponent!=0||(bits&0x7fffffu)==0),
          "Battle hit nonfinite/subnormal arithmetic is unsupported");
    return value;
}
class TowardZero {
public:
    TowardZero() {
        check(std::fegetenv(&saved_)==0,"Cannot save battle hit floating environment");
        if(std::fesetround(FE_TOWARDZERO)!=0) {
            std::fesetenv(&saved_);throw std::runtime_error("Cannot set battle hit rounding");
        }
    }
    ~TowardZero(){std::fesetenv(&saved_);}
private:
    std::fenv_t saved_{};
};
void impact(BattleActor& actor,const BattleHitServices& services) {
    const auto size=supported(std::bit_cast<float>(word(actor.storage,0x4c)));
    const int effect=size==5.0f?38:size==10.0f?39:40;
    check(bool(actor.model),"Battle hit impact requires live model channel1");
    auto point=actor.model->animation().value(1);
    TowardZero rounding;
    supported(point[1]);
    check(std::feclearexcept(FE_OVERFLOW|FE_UNDERFLOW)==0,"Cannot clear battle hit arithmetic flags");
    volatile float source_size=size;
    volatile float offset=source_size*0.5f;
    check(!(std::fetestexcept(FE_OVERFLOW|FE_UNDERFLOW)),"Battle hit size arithmetic overflow/underflow");
    supported(offset);
    volatile float y=point[1];
    volatile float adjusted=y-offset;
    check(!(std::fetestexcept(FE_OVERFLOW|FE_UNDERFLOW)),"Battle hit impact arithmetic overflow/underflow");
    point[1]=supported(adjusted);
    check(bool(services.positioned_effect),"Battle hit positioned effect1012f8 service missing");
    services.positioned_effect(effect,point,!actor.cartesian_motion);
}
}
int battle_apply_hit(BattleActor& actor,std::int16_t category,std::int16_t amount,
                     const BattleHitServices& services) {
    check(!actor.stat_bindings[1].empty(),"Battle hit primary status binding missing");
    if(actor.stat_bindings[1][0]==6) {
        //159c20: a dead target handles only revival categories6/7.
        if(category==6||category==7)
            throw std::runtime_error("Unrecovered battle hit primary status6 revival category "+std::to_string(category));
        return 0;
    }
    //159d00: SLTIU against the 0x3b-entry table at 0x2449c0; entries
    //6..9 and12 and all larger/negative categories branch to the return0.
    if(category<0||category>0x3a||(category>=6&&category<=9)||category==12)return 0;
    if(category==10||category==11) {
        check(bool(services.entry_escape_flag),"Battle hit 14ad10 escape flag service missing");
        if(services.entry_escape_flag()!=0)return 0;
        check(bool(services.command_available),"Battle hit 151978 command gate service missing");
        if(services.command_available()==0)return 0;
        check(bool(services.command_push),"Battle hit 1517e0 command push service missing");
        if(category==10){services.command_push(0,3);return 0;}
        services.command_push(3,4);
        //159f50: second table 0x244ab0 (amounts1..4); others return0.
        static constexpr std::array<std::int32_t,4> money{100,1000,10000,100000};
        if(amount>=1&&amount<=4) {
            check(bool(services.spend_money),"Battle hit 15abf8 money service missing");
            services.spend_money(money[unsigned(amount-1)]);
        }
        return 0;
    }
    if(category==0x38) {
        //15a774: BODY+14c (TASK18c) halfword increment, stored before the
        //signed >6 test, then clamped to6.
        const auto next=std::uint16_t(unsigned(actor.storage[0x18c])|(unsigned(actor.storage[0x18d])<<8))+1u;
        actor.storage[0x18c]=std::uint8_t(next);actor.storage[0x18d]=std::uint8_t(next>>8);
        if(std::bit_cast<std::int16_t>(std::uint16_t(next))>6){actor.storage[0x18c]=6;actor.storage[0x18d]=0;}
        return 0;
    }
    if(category!=0&&category!=1)
        throw std::runtime_error("Unrecovered battle hit category "+std::to_string(category));
    check(bool(services.attached_effect),"Battle hit attached effect1015e8 service missing");
    if(amount<=0) {
        services.attached_effect(36,actor);
        return 0;
    }
    const auto handle=services.attached_effect(35,actor);
    check(bool(services.effect_value),"Battle hit effect value1016a8 service missing");
    services.effect_value(handle,amount);
    check(bool(services.history),"Battle hit history15acd0 service missing");
    const auto delta=std::bit_cast<std::int16_t>(std::uint16_t(0u-std::uint32_t(amount)));
    services.history(actor,delta,category);
    // Source loads the live HP binding only after the history service.
    const auto remaining=word(actor.stat_bindings[0],0)-std::uint32_t(amount);
    put(actor.stat_bindings[0],0,remaining);
    int result;
    if(std::bit_cast<std::int32_t>(remaining)<=0) {
        put(actor.stat_bindings[0],0,0);
        check(bool(services.death_history),"Battle hit death history15ad88 service missing");
        services.death_history(actor);
        result=6;
    } else {
        put(actor.storage,0x128,4);
        check(bool(actor.model),"Battle hit survivor model owner missing");
        actor.model->select_motion(4);
        put(actor.storage,0x12c,std::bit_cast<std::uint32_t>(30.0f));
        result=10;
    }
    impact(actor,services);
    return result;
}
}
