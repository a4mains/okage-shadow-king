#include "okage/battle_actor_math.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
struct Rounding {
    int old=std::fegetround();
    explicit Rounding(int mode) {
        if(old<0||std::fesetround(mode))throw std::runtime_error("Battle actor rounding unavailable");
    }
    ~Rounding(){std::fesetround(old);}
};
void put16(std::span<std::uint8_t> b,unsigned at,std::uint16_t value) {
    b[at]=value;b[at+1]=value>>8;
}
void put32(std::span<std::uint8_t> b,unsigned at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)b[at+i]=value>>(8*i);
}
std::int16_t half(std::span<const std::uint8_t> b,unsigned at) {
    return __builtin_bit_cast(std::int16_t, (std::uint16_t(unsigned(b[at])|(unsigned(b[at+1])<<8))));
}
std::int32_t draw(const std::function<std::uint32_t()>& random) {
    return __builtin_bit_cast(std::int32_t, (random()))>>5;
}
}
void initialize_battle_actor_timer(std::span<std::uint8_t> b,int parameter,
        unsigned kind,int entry_half32,const std::function<std::uint32_t()>& random) {
    if(b.size()<0x14)throw std::runtime_error("Battle actor timer storage extent");
    const bool zero=kind!=1&&entry_half32==1;
    if(!zero&&!random)throw std::runtime_error("Battle actor timer requires shared RNG");
    float duration;
    {
        Rounding rounding(FE_TOWARDZERO);
        // 154600: preserve every single operation, including CVT.S.W.
        const float input=float(parameter);
        const float difference=100.f-input;
        const float square=difference*difference;
        const float quotient=square/66.f;
        const float added=quotient+10.f;
        duration=added*5.f;
    }
    put32(b,4,__builtin_bit_cast(std::uint32_t, (duration)));
    float initial=0.f;
    if(!zero) {
        const int r=kind==1?draw(random)%50:draw(random)%40+50;
        float product;
        {
            Rounding rounding(FE_TOWARDZERO);
            product=duration*float(r);
        }
        {
            Rounding rounding(FE_TONEAREST);
            const double scaled=double(product)*0.01;
            initial=float(scaled);
        }
    }
    put32(b,0,__builtin_bit_cast(std::uint32_t, (initial)));
    put32(b,8,0);put32(b,0x10,0x3f800000);
}
void initialize_battle_enemy_selector(std::span<std::uint8_t> b,
        std::int32_t current_value,int formation_count,
        const std::function<std::uint32_t()>& random) {
    if(b.size()<0x44)throw std::runtime_error("Battle enemy selector storage extent");
    if(formation_count<0||formation_count>65535)
        throw std::runtime_error("Battle formation count is not an unsigned halfword");
    if(!random)throw std::runtime_error("Battle enemy selector requires shared RNG");
    if((draw(random)&7)==0)put16(b,0x3c,std::uint16_t(draw(random)%9));
    std::int32_t result;
    switch(half(b,0x3c)) {
    case 1:result=draw(random)%3?1:0;break;
    case 2:result=draw(random)%3?5:6;break;
    case 3:result=draw(random)%5+1;break;
    case 4:result=0;break;
    case 5:result=6;break;
    case 6:result=draw(random)%3;break;
    case 7:result=formation_count-1;break;
    case 8:case 9: {
        const auto divisor=half(b,0x18);
        if(!divisor)throw std::runtime_error("Battle enemy selector source division by zero");
        const auto numerator=__builtin_bit_cast(std::int32_t, (std::uint32_t(current_value)*7u-1u));
        const auto quotient=std::uint32_t(std::int64_t(numerator)/divisor);
        result=__builtin_bit_cast(std::int32_t, (half(b,0x3c)==9?6u-quotient:quotient));
        break;
    }
    default:result=3;break;
    }
    // SH precedes signed LH and clamping: truncate before testing the range.
    const auto narrowed=__builtin_bit_cast(std::int16_t, (std::uint16_t(result)));
    put16(b,0x40,std::uint16_t(std::clamp<int>(narrowed,0,6)));
    put16(b,0x42,0);
}
}
