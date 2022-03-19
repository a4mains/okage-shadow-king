#include "okage/battle_capture.hpp"
#include <bit>
#include <cfenv>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
struct Rounding {
    int previous=std::fegetround();
    Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Battle capture rounding unavailable");}
    ~Rounding(){std::fesetround(previous);}
};
std::int32_t signed_word(std::uint32_t v){return __builtin_bit_cast(std::int32_t, (v));}
std::uint64_t extended(std::uint32_t v){return std::uint64_t(std::int64_t(signed_word(v)));}
std::uint64_t xyz(std::uint32_t x,std::uint32_t y) {
    return extended(x)|(extended(y)<<16)|0xffff00000000ull;
}
}
BattleCaptureDraw BattleCaptureLayer::draw(bool field_nonzero,const SineTable& sine) {
    Rounding rounding;
    BattleCaptureDraw result{mode,rgba,0x10000u-progress};
    switch(mode&255u) {
        case 1:progress+=0x80;break;
        case 2:
            if(result.scale==0x28000)++mode;
            mode+=0x100;progress-=0x400;break;
        case 3:mode-=0x1000;progress+=0x100;break;
    }
    const std::uint32_t cx=0x7ff0,cy=cx|(field_nonzero?0u:8u);
    // MIPS shift/add multiplication retains only the low signed 32 bits.
    const auto width=signed_word(result.scale*640u),height=signed_word(result.scale*224u);
    const auto half_width=width>>13,half_height=height>>13;
    const auto rotation=signed_word(result.mode)>>8;
    if(rotation==0) {
        const auto x=cx-std::uint32_t(half_width),y=cy-std::uint32_t(half_height);
        result.count=2;
        result.vertices[0]={0,xyz(x,y)};
        result.vertices[1]={0x0df027f0,xyz(x+std::uint32_t(width>>12)+16,y+std::uint32_t(height>>12)+16)};
    } else {
        const auto angle=(std::uint32_t(rotation)<<2)&0x3fff;
        const float s=-sine[angle],c=sine[(angle+0x1000)&0x3fff];
        const float ch=c*float(half_height),sh=s*float(half_height);
        const float sw=s*float(half_width),cw=(-c)*float(half_width);
        const float xs[]={-sw+cw,-sw-cw,sw-cw,sw+cw};
        const float ys[]={-ch+sh,-ch-sh,ch-sh,ch+sh};
        constexpr std::uint64_t uv[]={0,0x27f0,0x0df027f0,0x0df00000};
        result.count=4;
        for(unsigned i=0;i<4;++i)
            result.vertices[i]={uv[i],xyz(cx+std::uint32_t(std::int32_t(xs[i])),cy+std::uint32_t(std::int32_t(ys[i])))};
    }
    return result;
}
}
