#include "okage/enemy.hpp"
#include <cfenv>
#include <cmath>
#include <bit>
#include <limits>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
struct Rounding {
    int old=std::fegetround();
    Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Enemy rounding unavailable");}
    ~Rounding(){std::fesetround(old);}
};
Float4 direction(float turns,float scale,const SineTable& sine) {
    const float scaled=turns*32768.f;
    if(!std::isfinite(scaled)||scaled< -2147483648.f||scaled>=2147483648.f)
        throw std::runtime_error("Enemy angle conversion bounds");
    const auto index=(std::uint32_t(std::int32_t(scaled))>>1)&0x3fff;
    return {sine[index]*scale,0,sine[(index+4096)&0x3fff]*scale,0};
}
float distance(const Float4& a,const Float4& b){return std::fabs(a[0]-b[0])+std::fabs(a[2]-b[2]);}
}
std::int32_t EncounterEnemy::initial_cooldown(std::uint32_t random,unsigned flags) {
    return ((random>>5)&15)*80+((flags&2)?480:240);
}
EnemyActions EncounterEnemy::tick(const EnemyObservation& in,const std::function<std::uint32_t()>& random,
                                  const AtanTable& angles,const SineTable& sine) {
    Rounding rounding;EnemyActions out;
    if(!random||!std::isfinite(speed)||cooldown<0)throw std::runtime_error("Invalid enemy state/services");
    for(const auto* point:{&in.position,&in.rotation,&in.color,&in.player_position,&in.player_rotation,&in.camera_rotation})
        for(float v:*point)if(!std::isfinite(v)||std::fabs(v)>1e6f)throw std::runtime_error("Invalid enemy transform");
    if(counter==0) {
        if(cooldown){out.visible=false;--cooldown;return out;}
        if(((random()>>5)&511)>=3){out.visible=false;return out;}
        if(in.encounter_gate!=0||in.player_motion<=1)return out;
        // 0x137090 uses camera yaw and a second RNG value, not actor facing.
        const float yaw=float(int((random()>>5)&31)-16)*.005f+in.camera_rotation[1];
        auto position=in.player_position;const auto offset=direction(yaw,50,sine);
        for(unsigned i=0;i<4;++i)position[i]+=offset[i];
        out.position=position;counter=300;out.visible=true;out.translucent=true;
        out.color=Float4{.5f,.5f,.5f,0};return out;
    }
    const float alpha=in.color[3];
    if(counter<0) {
        if(speed> -1)speed-=.001f;
        const float faded=alpha-.02f;
        if(faded>0)out.alpha=faded;
        else {out.position=Float4{0,50,0,0};counter=0;}
        return out;
    }
    if(alpha<1)out.alpha=std::min(1.f,alpha+.02f);
    float separation=distance(in.player_position,in.position);
    const float facing=camera_angle(-(in.position[0]-in.player_position[0]),
                                   -(in.position[2]-in.player_position[2]),angles)+.5f;
    if(separation<6) {
        const float height=in.player_position[1]-in.position[1];
        if(height>4||height< -4)separation=99;
    }
    if(separation<6) {
        auto shifted=in.position;const auto offset=direction(in.player_rotation[1],.2f,sine);
        for(unsigned i=0;i<4;++i)shifted[i]+=offset[i];
        out.rear_attack=distance(in.player_position,shifted)-separation>.215f;
        if(in.field_mode!=0)return out;
        out.encounter_column=random()%24;out.field_mode=in.steps%20==0?8:11;
        counter=-1;return out;
    }
    out.rotation=Float4{0,facing,0,0};out.displacement=direction(facing,speed,sine);
    (*out.displacement)[3]=in.player_position[3]*speed;
    if(--counter==0)counter=-1;
    return out;
}
Float4 WanderingNpc::begin_leg(const Float4& position,float target_w,
                              const std::function<std::uint32_t()>& random,const AtanTable& angles) {
    Rounding rounding;
    if(counter>0||!random||!std::isfinite(target_w))throw std::runtime_error("Invalid wander leg initialization");
    for(float value:position)if(!std::isfinite(value))throw std::runtime_error("Invalid wander position");
    // 0x137eb8/0x137ee0: world-space destinations, two shared RNG calls.
    const auto x=random(),z=random();
    if(x>0x7fffffffu||z>0x7fffffffu)throw std::runtime_error("Invalid shared wander RNG result");
    const Float4 target{float(int(x%100)-50),0,float(int(z%100)-50),target_w};
    Float4 delta;for(unsigned k=0;k<4;++k)delta[k]=target[k]-position[k];
    const float distance=std::sqrt(delta[0]*delta[0]+delta[2]*delta[2]);
    if(!std::isfinite(distance)||distance>=67108864.f)throw std::runtime_error("Wander duration overflow");
    counter=std::int32_t(distance)*32;
    const float reciprocal=counter?1.f/float(counter):std::numeric_limits<float>::max();
    velocity=delta;for(unsigned k=0;k<3;++k)velocity[k]=delta[k]*reciprocal;
    Float4 rotation{camera_angle(-delta[1],distance,angles),camera_angle(delta[0],delta[2],angles),0,1};
    rotation[1]+=.5f;while(rotation[1]>1.f)rotation[1]-=1.f;
    return rotation;
}
void WanderingNpc::finish_move(const Float4& before,const Float4& after) {
    Rounding rounding;
    if(counter<=0)throw std::runtime_error("Wander movement without a live leg");
    Float4 delta;for(unsigned k=0;k<4;++k)delta[k]=before[k]-after[k];
    const float moved=(delta[0]*delta[0]+delta[1]*delta[1])+delta[2]*delta[2];
    if(moved<__builtin_bit_cast(float, (0x3a03126fu)))counter=0;
    --counter;
}
}
