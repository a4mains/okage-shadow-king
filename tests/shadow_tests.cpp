#include "okage/shadow.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <stdexcept>

void check(bool v) {if(!v)throw std::runtime_error("Shadow test failed");}
template<class F>void rejects(F f) {
    try{f();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Invalid shadow state accepted");
}
int main(){try{
    const okage::Float4 rotation{.125f,-.25f,.03125f,1},position{10,20,30,1};
    okage::ShadowTransition s;s.phase=1;s.factor=1;s.locked=1;
    const int previous=std::fegetround();std::fesetround(FE_UPWARD);
    for(int i=0;i<20;++i)s.tick(rotation,position);
    check(std::fegetround()==FE_UPWARD);
    // These boundary words come from original0x135100 execution, not a timer.
    check(s.phase==1&&__builtin_bit_cast(unsigned, (s.factor))==0xb4600000u);
    check(s.hero_shadow==4&&!s.stun_visible&&s.locked==1);
    s.tick(rotation,position);
    check(s.phase==2&&s.factor==0&&s.hero_shadow==0xffff&&!s.stun_visible);
    s.tick(rotation,position);check(s.stun_visible&&s.phase==2);
    for(int i=0;i<20;++i)s.tick(rotation,position);
    check(s.phase==2&&__builtin_bit_cast(unsigned, (s.factor))==0x3f866661u);
    s.tick(rotation,position);check(s.phase==3&&s.factor==1&&s.projection_factor==0);
    check(s.stun_scale==okage::Float4({1,1,1,1})&&s.stun_rotation==rotation);
    check(s.stun_position[0]==10&&s.stun_position[1]<20&&s.stun_position[2]==30);
    const auto frozen=s.stun_position;s.follow_disabled=1;
    s.phase=4;s.factor=0;s.tick({},{100,100,100,1});
    check(s.phase==5&&!s.stun_visible&&s.stun_position==frozen&&s.hero_shadow==0xffff);
    s.tick({},{});check(s.hero_shadow==4&&s.phase==5);
    s.factor=1;s.tick({},{});check(s.phase==0&&s.projection_factor==1);
    s.phase=6;const auto before=s;
    rejects([&]{s.tick(rotation,position);});
    check(s.phase==before.phase&&s.stun_position==before.stun_position);
    check(std::fegetround()==FE_UPWARD);
    s.phase=0;s.factor=std::numeric_limits<float>::quiet_NaN();
    rejects([&]{s.tick(rotation,position);});
    s.factor=0;auto bad=position;bad[1]=std::numeric_limits<float>::infinity();
    rejects([&]{s.tick(rotation,bad);});
    check(s.stun_position==before.stun_position);
    std::fesetround(previous);
    std::cout<<"Shadow boundaries, follow, lock, rounding and invalid-state checks pass\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
