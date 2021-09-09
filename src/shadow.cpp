#include "okage/shadow.hpp"
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
void ShadowTransition::tick(const Float4& hero_rotation,const Float4& hero_position) {
    if(phase>5||!std::isfinite(factor)||!std::isfinite(projection_factor))
        throw std::runtime_error("Unrecovered shadow transition state");
    for(const auto& v:{hero_rotation,hero_position,stun_rotation,stun_position,stun_scale})
        for(float f:v)if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow transform");
    struct Rounding {
        int old=std::fegetround();
        Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Cannot set shadow rounding");}
        ~Rounding(){std::fesetround(old);}
    } rounding;
    if(!follow_disabled) {
        stun_rotation=hero_rotation;stun_position=hero_position;
        stun_position[1]-=.01f;
    }
    const auto shrink=[&] {if(factor>0){factor-=.05f;return false;}factor=0;return true;};
    const auto grow=[&] {if(factor<1){factor+=.05f;return false;}factor=1;return true;};
    switch(phase) {
    case 0:case 3:break;
    case 1:
        if(shrink()){phase=2;hero_shadow=0xffff;}
        projection_factor=factor;break;
    case 2:
        if(grow())phase=3;
        stun_scale={factor,factor,factor,1};stun_visible=true;break;
    case 4:
        if(shrink()){phase=5;stun_visible=false;}
        stun_scale={factor,factor,factor,1};break;
    case 5:
        if(grow())phase=0;
        projection_factor=factor;hero_shadow=4;break;
    }
}
}
