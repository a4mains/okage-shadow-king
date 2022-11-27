#include "okage/ui_projection.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
namespace {
void check(bool v){if(!v)throw std::runtime_error("UI projection test failure");}
template<class F> void rejects(F f){bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}check(caught);}
using namespace okage;
Matrix4 identity(){return {{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};}
UiProjectionParameters orthographic(){
    return {{{1,1,1,0},{0,0,0,1},{1,1,1,0},{0,0,0,0},
        {2048,2048,64,0},{320,112,32,128},{1728,1936,-1,0},{2368,2160,1,1}}};
}
std::array<UiProjectionVertex,4> quad(){
    std::array<UiProjectionVertex,4> out;
    const std::array<Float4,4> pos={Float4{-.5f,-.5f,0,1},Float4{.5f,-.5f,0,1},
        Float4{.5f,.5f,0,1},Float4{-.5f,.5f,0,1}};
    for(unsigned i=0;i<4;++i)out[i]={pos[i],{1.f,.25f,2.f,1.f},{128,128,128,128},{.25f,.75f}};
    return out;
}
void basics(){
    const auto p=orthographic();auto q=quad();
    const auto out=ui_project_quad(p,identity(),q,true);check(out.emitted);
    check(out.vertices[0].clip==q[0].position);
    check(out.vertices[0].screen==Float4({1888,1992,64,128}));
    check(out.vertices[0].xyzf==std::array<std::uint32_t,4>({30208,31872,1024,2048}));
    check(out.vertices[0].rgba==std::array<std::uint32_t,4>({0x4b000080,0x4b000020,0x4b0000ff,0x4b000080}));
    check(out.vertices[0].stq==std::array<float,3>({.25f,.75f,1}));
    std::swap(q[1],q[3]);check(ui_project_quad(p,identity(),q,true).emitted); //Mode0 huge bias accepts both windings.
    q[0].uv[0]=std::numeric_limits<float>::quiet_NaN();
    const auto noTexture=ui_project_quad(p,identity(),q,false);check(noTexture.emitted);
    check(noTexture.vertices[0].stq==std::array<float,3>{});
    rejects([&]{ui_project_quad(p,identity(),q,true);});
}
void fractional_and_depth(){
    auto p=orthographic();auto q=quad();
    p[4][0]=2048.03125f;
    p[6][3]=.25f;p[7][3]=2;
    for(auto& v:q)v.source_color={.50390625f,1.f,1.f,1.f};
    const auto out=ui_project_quad(p,identity(),q,true);
    check(out.vertices[0].xyzf[0]==30208);
    check(out.vertices[0].rgba[0]==0x4b000040); //64.5→64 under bias packing.
    check(out.vertices[0].stq==std::array<float,3>({4.5f,13.5f,18.f})); //Q=screenZ*.25+2, not reciprocal clipW.
    p[4][2]=134217728.f;
    check(ui_project_quad(p,identity(),q,false).vertices[0].xyzf[2]==0x7fffffff);
    p[4][2]=-2;p[5][3]=300;
    const auto clamped=ui_project_quad(p,identity(),q,false);
    check(clamped.vertices[0].screen[2]==0&&clamped.vertices[0].xyzf[2]==0);
    check(clamped.vertices[0].xyzf[3]==255*16);
}
void composition_and_clipping(){
    auto p=orthographic();auto q=quad();auto m=identity();
    p[0][0]=.3f;m[0][0]=.3f;m[1][0]=.3f;m[1][1]=0;
    for(auto& v:q)v.position={.3f,1.1f,0,1};
    check(__builtin_bit_cast(std::uint32_t, (ui_project_quad(p,m,q,false).vertices[0].clip[0]))==0x3e010624);
    p=orthographic();m=identity();
    p[0]={3,5,7,2};p[1]={11,13,17,19};m[3]={1,2,3,4};
    for(auto& v:q)v.position={0,0,0,1};
    const auto rejected=ui_project_quad(p,m,q,false);
    check(!rejected.emitted);check(rejected.vertices[0].clip==Float4({47,62,89,82}));
    check(rejected.vertices[0].clip_flags==16);
    p=orthographic();q=quad();
    for(auto& v:q)v.position[0]=2;
    check(!ui_project_quad(p,identity(),q,true).emitted);
    q=quad();q[0].position[0]=-2;
    rejects([&]{ui_project_quad(p,identity(),q,true);}); //No silently missing partial polygon.
    q=quad();for(auto& v:q)v.position[0]=1;
    check(ui_project_quad(p,identity(),q,false).emitted);
}
void delay_slot_column2(){
    auto p=orthographic();auto m=identity();auto q=quad();
    p[0]={3,5,7,2};p[1]={11,13,17,19};
    m[2]={.125f,-.25f,.5f,1};
    for(auto& v:q)v.position={0,0,1,0};
    const auto out=ui_project_quad(p,m,q,false);
    check(!out.emitted);
    for(const auto& v:out.vertices){
        check(v.clip==Float4({11.375f,11.75f,20.5f,20}));
        check(v.clip_flags==16);
        check(v.clip!=m[2]);
    }
    // A nondegenerate, emitted quad also uses the projected Z column.
    p=orthographic();p[0][2]=.5f;m=identity();q=quad();
    for(auto& v:q)v.position[2]=.5f;
    const auto visible=ui_project_quad(p,m,q,false);
    check(visible.emitted);
    for(const auto& v:visible.vertices){
        check(v.clip[2]==.25f);
        check(v.screen[2]==72.f);
        check(v.xyzf[2]==1152);
    }
}
void cache_and_errors(){
    check(ui_projection_color_cache({2,3,4,5},{4,3,2,1},{1,2,3,4})==Float4({9,11,11,9}));
    check(ui_projection_uv_cache({.25f,.5f},{2,4},{1,-1})==std::array<float,2>({1.5f,1}));
    const auto zero=ui_projection_color_cache({-0.f,0,0,0});check(__builtin_bit_cast(std::uint32_t, (zero[0]))==0);
    auto p=orthographic();auto q=quad();
    p[1][3]=0;rejects([&]{ui_project_quad(p,identity(),q,false);});
    p=orthographic();q[0].source_color[0]=-1;rejects([&]{ui_project_quad(p,identity(),q,false);});
    q=quad();q[0].position[0]=std::numeric_limits<float>::infinity();rejects([&]{ui_project_quad(p,identity(),q,false);});
    q=quad();q[0].position[0]=std::numeric_limits<float>::denorm_min();rejects([&]{ui_project_quad(p,identity(),q,false);});
    rejects([]{ui_projection_color_cache({std::numeric_limits<float>::max(),0,0,0},{2,1,1,1});});
    rejects([]{ui_projection_uv_cache({std::numeric_limits<float>::min(),0},{.5f,1});});
}
}
int main(){try{
    std::fenv_t original;check(std::fegetenv(&original)==0);
    std::array<UiProjectedVertex,4> expected{};bool first=true;
    for(int mode:{FE_TONEAREST,FE_TOWARDZERO,FE_UPWARD,FE_DOWNWARD}){
        check(std::fesetround(mode)==0);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_INVALID|FE_INEXACT);
        const auto flags=std::fetestexcept(FE_ALL_EXCEPT);
        const auto out=ui_project_quad(orthographic(),identity(),quad(),true);
        if(first){expected=out.vertices;first=false;}else for(unsigned i=0;i<4;++i){
            check(out.vertices[i].xyzf==expected[i].xyzf&&out.vertices[i].rgba==expected[i].rgba);
            for(unsigned j=0;j<4;++j)check(__builtin_bit_cast(std::uint32_t, (out.vertices[i].screen[j]))==__builtin_bit_cast(std::uint32_t, (expected[i].screen[j])));
        }
        check(std::fegetround()==mode&&std::fetestexcept(FE_ALL_EXCEPT)==flags);
        rejects([]{ui_projection_color_cache({std::numeric_limits<float>::max(),0,0,0},{2,1,1,1});});
        check(std::fegetround()==mode&&std::fetestexcept(FE_ALL_EXCEPT)==flags);
    }
    std::fesetenv(&original);basics();fractional_and_depth();composition_and_clipping();delay_slot_column2();cache_and_errors();
    std::cout<<"UI projection tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
