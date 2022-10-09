#include "okage/ui_window.hpp"
#include "okage/ui_layout_geometry.hpp"
#include <array>
#include <bit>
#include <cfenv>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
void check(bool v,const char* message) {if(!v)throw std::runtime_error(message);}
float ordinary(float f) {
    const auto bits=__builtin_bit_cast(std::uint32_t, (f)),exponent=bits&0x7f800000u;
    check(exponent!=0x7f800000u&&(exponent!=0||(bits&0x7fffffu)==0),
          "UI sprite exceptional float");
    return f;
}
struct Rounding {
    std::fenv_t saved;
    Rounding() {
        check(std::fegetenv(&saved)==0,"UI sprite save fenv");
        if(std::fesetround(FE_TOWARDZERO)) {
            std::fesetenv(&saved);throw std::runtime_error("UI sprite rounding");
        }
    }
    ~Rounding() {std::fesetenv(&saved);}
};
float number(std::uint32_t bits) {
    // MTC1 then CVT.S.W, not reinterpretation as packed float bits.
    volatile auto input=__builtin_bit_cast(std::int32_t, (bits));
    volatile float output=static_cast<float>(input);return output;
}
std::uint32_t bits(float f) {return __builtin_bit_cast(std::uint32_t, (f));}
}
bool UiWindow::place_sprite(unsigned group,unsigned slot,const UiLayoutSprite& sprite) {
    check(group<word(0x3c04)&&slot<16,"UI sprite placement bounds");
    const std::size_t offset=(std::size_t(group)*16+slot)*16;
    check(offset+16<=sprite_[0].size()&&offset+16<=sprite_[1].size(),
          "UI sprite placement storage bounds");
    const auto sx=ordinary(sprite.style.scale_x),sy=ordinary(sprite.style.scale_y);
    std::array<float,4> old{};
    for(unsigned i=0;i<4;++i)
        old[i]=ordinary(__builtin_bit_cast(float, (sprite_[1].word(offset+i*4))));
    const auto color=ui_layout_color_rgba(sprite.style.color);
    const auto x=std::uint32_t(sprite.x)+word(0x234);
    const auto y=std::uint32_t(sprite.y)+word(0x238);
    Rounding rounding;
    const auto dimension=number((std::uint32_t(sprite.width)<<16)+std::uint32_t(sprite.height));
    const auto uv=number((std::uint32_t(sprite.u)<<16)+std::uint32_t(sprite.v));
    const auto px=number(x),py=number(y);
    std::array<std::uint32_t,4> six{};
    for(unsigned i=0;i<4;++i)six[i]=((color>>(8*i))&255u)>>2;
    const auto packed=six[0]+(six[1]<<6)+(six[2]<<12)+(six[3]<<18);
    const auto packed_float=number(packed);
    std::array<std::uint32_t,4> rgba{};
    for(unsigned i=0;i<4;++i) {
        // 19c8cc..19c920: CVT(channel6<<2), then multiply by0x3c000000.
        volatile float component=number(six[i]<<2),factor=0.0078125f;
        volatile float result=component*factor;rgba[i]=bits(result);
    }
    const std::array<float,4> next{sx,sy,34.f,uv};
    put(0x224,bits(sx));put(0x228,bits(sy));put(0x22c,color);
    sprite_[0].put_word(offset,bits(px));
    sprite_[0].put_word(offset+4,bits(py));
    sprite_[0].put_word(offset+8,bits(dimension));
    // Source stores R,G,A,B in that order (19c924..19c938).
    for(auto i:{0u,1u,3u,2u})put(0x3e40+offset+i*4,rgba[i]);
    sprite_[0].put_word(offset+12,bits(packed_float));
    bool dirty=false;
    for(unsigned i=0;i<4;++i) {
        if(old[i]!=next[i]) {sprite_[1].put_word(offset+i*4,bits(next[i]));dirty=true;}
    }
    return dirty;
}
}
