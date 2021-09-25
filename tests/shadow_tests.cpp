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
    const okage::Matrix4 camera{{{0,0,-1,0},{0,1,0,0},{1,0,0,0},{10,-7,3,1}}};
    const okage::ShadowProjectionContext projection({.3f,1,0,0},camera);
    check(std::fegetround()==FE_UPWARD);
    const auto exact=projection.project(4,80,1);
    const auto neighbor=projection.project(4,80,__builtin_bit_cast(float, (0x3f7fffffu)));
    check(exact.has_matrices&&!exact.direction&&neighbor.has_matrices&&neighbor.direction);
    check(__builtin_bit_cast(unsigned, (exact.matrices[1][3][0]))==0);
    check(__builtin_bit_cast(unsigned, (neighbor.matrices[1][3][0]))==0xbe931f40u);
    check(__builtin_bit_cast(unsigned, ((*neighbor.direction)[2]))==0xbe931f43u);
    check(__builtin_bit_cast(unsigned, (exact.matrices[1][0][0]))==0x3f7fffffu);
    check(__builtin_bit_cast(unsigned, (neighbor.matrices[1][0][0]))==0x3f800000u);
    for(unsigned mode=0;mode<8;++mode) {
        const auto output=projection.project(mode,80,1,{17,31,192});
        check(output.has_matrices==(mode>=4)&&!output.direction);
        check(output.color==((mode&2)?std::array<unsigned,3>{17,31,192}:std::array<unsigned,3>{}));
    }
    check(std::fegetround()==FE_UPWARD);
    rejects([&]{projection.project(8,0,1);});
    rejects([&]{projection.project(4,std::numeric_limits<float>::infinity(),1);});
    rejects([&]{projection.project(4,0,std::numeric_limits<float>::quiet_NaN());});
    rejects([&]{projection.project(2,0,1,{256,0,0});});
    // Failures both before and inside the rounding scope must restore the caller.
    auto invalid_camera=camera;invalid_camera[0][3]=1;
    rejects([&]{okage::ShadowProjectionContext invalid({.3f,1,0,0},invalid_camera);});
    rejects([&]{okage::ShadowProjectionContext invalid({0,0,0,0},camera);});
    rejects([&]{okage::ShadowProjectionContext invalid({1,0,0,0},camera);});
    check(std::fegetround()==FE_UPWARD);
    okage::ShadowPrismInput prism;
    prism.direction={0,1,0,0};
    prism.camera={okage::Float4{0,0,3,0},{1,0,3,0},{0,0,4,0},
                  {0,1,3,0},{1,1,3,0},{0,1,4,0}};
    prism.screen={okage::Float4{1024,1024,10000,128},{1280,1024,10000,128},
                  {1024,1280,10000,128},{1024,924,10050,128},
                  {1280,924,10050,128},{1024,1180,10050,128}};
    const auto draw=okage::shadow_prism(prism);
    check(draw.emitted&&draw.passes[0]==draw.passes[2]);
    check(draw.passes[0][0][2]==0x26480&&draw.passes[1][0][2]==0x27d80);
    check(draw.passes[0][6][3]==0x8800&&draw.passes[1][6][3]==0x800);
    check(draw.passes[0][3][3]==0x8800&&draw.passes[1][3][3]==0x8800);
    check(std::fegetround()==FE_UPWARD);
    prism.direction[1]=-1;check(!okage::shadow_prism(prism).emitted);prism.direction[1]=1;
    prism.screen[0][0]=0;check(okage::shadow_prism(prism).emitted);
    prism.clip[0]=1;check(!okage::shadow_prism(prism).emitted);
    prism.screen[0][0]=16;check(!okage::shadow_prism(prism).emitted);
    prism.screen[0][0]=17;check(okage::shadow_prism(prism).emitted);
    prism.clip[0]=16;check(!okage::shadow_prism(prism).emitted);prism.clip[0]=0;
    prism.clip_status=-1;check(!okage::shadow_prism(prism).emitted);prism.clip_status=0;
    prism.screen[0][2]=std::numeric_limits<float>::max();
    rejects([&]{okage::shadow_prism(prism);});check(std::fegetround()==FE_UPWARD);
    prism.screen[0][2]=10000;prism.camera[0][0]=std::numeric_limits<float>::infinity();
    rejects([&]{okage::shadow_prism(prism);});check(std::fegetround()==FE_UPWARD);
    std::fesetround(previous);
    std::cout<<"Shadow boundaries, follow, lock, projection, rounding and invalid-state checks pass\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
