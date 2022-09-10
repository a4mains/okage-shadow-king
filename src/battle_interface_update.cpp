#include "okage/battle_interface.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
struct Rounding {
    int old=std::fegetround();
    Rounding() {
        if(old<0||std::fesetround(FE_TOWARDZERO))
            throw std::runtime_error("Battle interface rounding unavailable");
    }
    ~Rounding(){std::fesetround(old);}
};
float checked(float f) {
    if(!std::isfinite(f)||std::fpclassify(f)==FP_SUBNORMAL)
        throw std::runtime_error("Unrecovered battle interface float class");
    return f;
}
float rotate(float f) {
    checked(f);
    f=checked(f+__builtin_bit_cast(float, (0x3d567451u)));
    const float lower=__builtin_bit_cast(float, (0xc0490d0cu)),
                upper=__builtin_bit_cast(float, (0x40490d0cu)),
                period=__builtin_bit_cast(float, (0x40c90d0cu));
    while(f<lower) {
        const float next=checked(f+period);
        if(next==f)throw std::runtime_error("Battle interface angle wrap cannot advance");
        f=next;
    }
    while(upper<f) {
        const float next=checked(f-period);
        if(next==f)throw std::runtime_error("Battle interface angle wrap cannot advance");
        f=next;
    }
    return f;
}
}
BattleUiPadMasks BattleInterface::first_update(std::shared_ptr<RuwResource> resource,
    BattleUiPad& pad,const BattleUiPadInput& input,unsigned display_rate) {
    if(!resource)throw std::runtime_error("Battle interface requires owned RUW slot0");
    Rounding rounding;
    BattleUiPadMasks masks{};
    words_[0xe8c/4]=0;
    for(unsigned layer=0;layer<words_[0xe40/4];++layer) {
        if(layer>=handles_.size())throw std::runtime_error("Battle interface layer extent");
        const unsigned base=(0x40+layer*0xe0)/4;
        if(!(words_[base+0x50/4]&0x40))continue;
        words_[0xe48/4]=layer;
        words_[base+0xd4/4]=1;
        auto& cursor=words_[0xe8c/4];
        if(cursor>=16)throw std::runtime_error("Battle interface state cursor outside owner");
        auto& state=words_[0xe4c/4+cursor];
        const int handle=handles_.at(layer);
        if(handle<0||unsigned(handle)>=shared_.registry.size()||!shared_.registry[unsigned(handle)])
            throw std::runtime_error("Battle interface callback window lifetime");
        auto& w=*shared_.registry[unsigned(handle)];
        const auto bind=[&](std::span<const std::size_t> groups) {
            shared_.resource_accumulators[0]=0;
            w.bind_ruw(resource,groups);
        };
        const auto transform=[&] {
            std::array<std::uint32_t,3> position,rotation;
            std::copy_n(words_.begin()+base+0x10/4,3,position.begin());
            std::copy_n(words_.begin()+base+0x20/4,3,rotation.begin());
            w.set_transform_xyz(position,rotation);
        };
        switch(words_[base+0x60/4]) {
        case 2: {
            // 165b50 always executes this prefix, including states 0/-100.
            masks=pad.normalize(input);
            if(shared_.hud.selection_state>50)masks={};
            switch(__builtin_bit_cast(std::int32_t, (state))) {
            case 0: {
                pad.set_enabled(0,true);pad.set_repeat(0,30,15,display_rate);
                words_[base+0x64/4]=0xffffffff;
                words_[base+0x64/4]=0;
                words_[base+0x64/4]=19;
                w.set_opacity_tween(-128.f,128.f,0,128);
                constexpr std::array<std::size_t,3> groups{2,3,4};bind(groups);
                ++state;state=__builtin_bit_cast(std::uint32_t, (std::int32_t(-100)));
                break; // Unlike type3, no same-call fallthrough.
            }
            case -100:
                words_[base+0x64/4]=0;words_[base+0x68/4]=0;
                words_[base+0x64/4]=0;state=2;break;
            case -1:w.set_opacity_tween(0.f,0.f,0,128);break;
            case 2:
                words_[base+0x64/4]=0xffffffff;words_[base+0x68/4]=0;
                if(w.opacity()!=0.f)
                    throw std::runtime_error("Battle type2 interactive input branch unrecovered");
                break;
            default:
                throw std::runtime_error("Battle type2 state unrecovered: "+
                    std::to_string(__builtin_bit_cast(std::int32_t, (state))));
            }
            ++cursor;break;
        }
        case 3:
            if(state==0) {
                words_[base+0x64/4]=0xffffffff;words_[base+0x64/4]=0;
                constexpr std::array<std::size_t,1> groups{5};bind(groups);
                w.set_opacity_tween(-128.f,128.f,0,128);
                words_[base+0x64/4]=0;++state;
            }
            if(state==1) {
                float angle=rotate(__builtin_bit_cast(float, (words_[base+0x24/4])));
                words_[base+0x24/4]=__builtin_bit_cast(std::uint32_t, (angle));
                if(angle>0.f) {
                    words_[base+0x24/4]=0;
                    words_[base+0x64/4]=0;words_[base+0x68/4]=0;
                    words_[base+0x64/4]=0;++state;
                }
                transform();
            } else if(state==0xffffffffu)w.set_opacity_tween(-4.f,128.f,0,128);
            ++cursor;break;
        default:throw std::runtime_error("Battle interface first callback type unrecovered");
        }
        if(__builtin_bit_cast(std::int32_t, (cursor))>15)cursor=0;
    }
    return masks;
}
}
