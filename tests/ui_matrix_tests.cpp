#include "okage/ui_matrix.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
namespace {
using namespace okage;
void check(bool v){if(!v)throw std::runtime_error("UI matrix test failure");}
template<class F> void rejects(F f){bool thrown=false;try{f();}catch(const std::runtime_error&){thrown=true;}check(thrown);}
bool same(const Matrix4& a,const Matrix4& b){for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)
    if(__builtin_bit_cast(std::uint32_t, (a[c][r]))!=__builtin_bit_cast(std::uint32_t, (b[c][r])))return false;return true;}
void order(const SineTable& sine){
    const auto sprite=ui_sprite_parent_matrix({2,3,4},{0,0,.25f},true,sine);
    const auto glyph=ui_glyph_parent_matrix({2,3,4},{0,0,.25f},true,sine);
    check(sprite[3]==Float4({-3,2,4,1}));check(glyph[3]==Float4({2,3,4,1}));
    for(unsigned i=0;i<3;++i)check(sprite[i]==glyph[i]);
    const auto zxy=ui_matrix_rotate_zxy(ui_matrix_identity(),{.25f,.25f,.25f},sine);
    check(zxy[0]==Float4({-1,0,0,0}));check(zxy[1]==Float4({0,0,1,0}));check(zxy[2]==Float4({0,1,0,0}));
    const auto inverse=ui_matrix_rotate_zxy(ui_matrix_identity(),{0,0,-.25f},sine);
    check(inverse[0]==Float4({0,-1,0,0}));check(inverse[1]==Float4({1,0,0,0}));
    auto parent=ui_matrix_identity();parent[0][0]=2;parent[1][1]=3;parent[2][2]=4;parent[3]={10,20,30,1};
    const auto element=ui_sprite_element_matrix(parent,{1,2,3},sine);
    check(element[3]==Float4({12,26,42,1}));
    const Matrix4 arbitrary{{{1,2,3,4},{5,6,7,8},{9,10,11,12},{13,14,15,16}}};
    auto local=ui_matrix_identity();local[0]={1,2,3,4};
    check(ui_matrix_left_multiply(arbitrary,local)[0]==Float4({90,100,110,120}));
}
void zeros_and_quantization(const SineTable& sine){
    Matrix4 negative{};for(auto& c:negative)for(auto& v:c)v=-0.f;
    const auto result=ui_matrix_rotate_zxy(negative,{0,0,0},sine);
    for(unsigned r=0;r<4;++r){
        check(__builtin_bit_cast(std::uint32_t, (result[0][r]))==0);
        check(__builtin_bit_cast(std::uint32_t, (result[1][r]))==0);
        check(__builtin_bit_cast(std::uint32_t, (result[2][r]))==0x80000000);
        check(__builtin_bit_cast(std::uint32_t, (result[3][r]))==0x80000000);
    }
    auto table=sine;
    table[1]=.5f;table[4097]=.75f;
    const auto low=ui_matrix_rotate_zxy(ui_matrix_identity(),{0,0,1.f/32768.f},table);
    const auto high=ui_matrix_rotate_zxy(ui_matrix_identity(),{0,0,2.f/32768.f},table);
    check(low[0][0]==1&&low[0][1]==0);check(high[0][0]==.75f&&high[0][1]==.5f);
    // Saturation occurs before the wrapping sine index.
    table[16383]=1;table[4095]=0;
    const auto saturated=ui_matrix_rotate_zxy(ui_matrix_identity(),{0,0,65536.f},table);
    check(saturated[0]==Float4({0,1,0,0}));
    const auto minimum=ui_matrix_rotate_zxy(ui_matrix_identity(),{0,0,-65536.f},table);
    check(minimum[0]==Float4({1,0,0,0}));
}
void guarded(const SineTable& sine){
    const auto nan=std::numeric_limits<float>::quiet_NaN();
    // Branch-disabled angles/table are genuinely not consumed by source.
    check(ui_sprite_parent_matrix({1,2,3},{nan,nan,nan},false,{})[3]==Float4({1,2,3,1}));
    rejects([&]{ui_glyph_parent_matrix({1,2,3},{nan,0,0},true,sine);});
    rejects([&]{ui_sprite_element_matrix(ui_matrix_identity(),{0,0,0},{});});
    rejects([&]{ui_matrix_translate(ui_matrix_identity(),{std::numeric_limits<float>::denorm_min(),0,0});});
    auto table=sine;table[4096]=std::numeric_limits<float>::infinity();
    rejects([&]{ui_sprite_element_matrix(ui_matrix_identity(),{0,0,0},table);});
    auto large=ui_matrix_identity();large[0][0]=std::numeric_limits<float>::max();
    rejects([&]{ui_matrix_translate(large,{2,0,0});});
    auto small=ui_matrix_identity();small[0][0]=std::numeric_limits<float>::min();
    rejects([&]{ui_matrix_translate(small,{.5f,0,0});});
}
}
int main(){try{
    const auto sine=make_sine_table();order(sine);zeros_and_quantization(sine);guarded(sine);
    std::fenv_t saved;check(std::fegetenv(&saved)==0);Matrix4 expected{};bool first=true;
    for(int mode:{FE_TONEAREST,FE_TOWARDZERO,FE_UPWARD,FE_DOWNWARD}){
        std::fesetround(mode);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_INVALID|FE_INEXACT);
        const auto flags=std::fetestexcept(FE_ALL_EXCEPT);
        const auto m=ui_sprite_element_matrix(ui_sprite_parent_matrix({.1f,.2f,.3f},{.17f,-.13f,.09f},true,sine),{.6f,.7f,.8f},sine);
        if(first){expected=m;first=false;}else check(same(expected,m));
        check(std::fegetround()==mode&&std::fetestexcept(FE_ALL_EXCEPT)==flags);
        auto large=ui_matrix_identity();large[0][0]=std::numeric_limits<float>::max();
        rejects([&]{ui_matrix_translate(large,{2,0,0});});
        check(std::fegetround()==mode&&std::fetestexcept(FE_ALL_EXCEPT)==flags);
    }
    std::fesetenv(&saved);std::cout<<"UI matrix tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
