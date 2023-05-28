#include "okage/battle_hit.hpp"
#include <bit>
#include <cfenv>
#include <stdexcept>

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
    services.positioned_effect(effect,point);
}
}
int battle_apply_hit(BattleActor& actor,std::int16_t category,std::int16_t amount,
                     const BattleHitServices& services) {
    check(!actor.stat_bindings[1].empty(),"Battle hit primary status binding missing");
    check(actor.stat_bindings[1][0]!=6,"Unrecovered battle hit primary status6 branch");
    check(category==0||category==1,"Unrecovered battle hit category");
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
