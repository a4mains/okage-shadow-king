#include "okage/field.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <stdexcept>
void require(bool value){if(!value)throw std::runtime_error("Field test failed");}
template<class F> void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Expected field rejection");}
int main(){try {
    const auto angles=okage::make_atan_table();const auto sine=okage::make_sine_table();
    require(okage::camera_angle(0,0,angles)==0);
    require(okage::camera_angle(1,0,angles)==.25f);
    require(okage::camera_angle(-1,0,angles)==.75f);
    const auto look=okage::camera_look_at({7.75f,-6.71f,-20.12f,1},{-12.35f,-2,-4.35f,1},angles,sine);
    require(look[0]==.97100830078125f && look[1]==.8558349609375f); // Original execution.
    okage::CameraBlend blend;okage::Float4 p{10,4,2,1},t{1,2,3,1};
    blend.begin(.5f,{0,0,0,1},{0,0,0,1});blend.apply(p,t);require(p[0]==0 && blend.phase==.5f);
    p={10,4,2,1};t={1,2,3,1};blend.apply(p,t);require(p[0]==5 && t[0]==.5f && blend.phase==1);
    const int rounding=std::fegetround();okage::camera_angle(1,3,angles);require(std::fegetround()==rounding);
    std::vector<std::uint8_t> b(0xc00);
    auto word=[&](unsigned at,std::uint32_t v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(i*8);};
    auto point=[&](unsigned at,okage::Float4 v){for(unsigned i=0;i<3;++i)word(at+4*i,__builtin_bit_cast(std::uint32_t, (v[i])));};
    word(0,0x544d58);word(4,b.size()-32);word(16,0xffffffff);
    word(0xb88,0xffe00007);word(0xb8c,0x30);point(0xb90,{2,3,4,1});
    word(0xb9c,0x10004b);point(0xba0,{0,0,0,1});word(0xbac,0x3c00002);
    word(0xbb0,0x4c);word(0xbb4,10100);word(0xbb8,0x20000a);word(0xbbc,0xffe00007);
    word(0xbc0,0x5a00033);point(0xbc4,{8,6,4,1});word(0xbd0,0x5a00052);
    word(0xbd4,0x10004b);point(0xbd8,{1,2,3,1});word(0xbe4,0xff800236);
    unsigned frame=0,starts=0,polls=0;
    okage::OpeningField field(b,[&](int id){require(id==10100);++starts;},[&](int n){require(n==2);++polls;return frame>=70;});
    for(frame=1;frame<=180;++frame){field.tick();if(frame<70)require(field.position[0]==2);}
    require(starts==1 && polls==10 && field.pc()==0xbe4 && field.pending_opcode==0x36);
    require(field.pending_actor==-8 && field.pending_animation==2 && field.position[0]==8 && field.target[0]==1);
    word(0xb88,0);rejects([&]{okage::OpeningField bad(b,[](int){},[](int){return true;});});
    rejects([&]{okage::field_projection({});});
    std::cout<<"Camera look-at, blend, field dialogue gate and pending actor service passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
