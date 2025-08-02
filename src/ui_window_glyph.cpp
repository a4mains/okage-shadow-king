#include "okage/rng_site.hpp"
#include "okage/ui_window.hpp"
#include "okage/font.hpp"
#include <array>
#include <bit>
#include <cfenv>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
using I=std::int32_t;
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
I si(std::uint32_t value) {return std::bit_cast<I>(value);}
I add(I a,I b) {return si(std::uint32_t(a)+std::uint32_t(b));}
I sub(I a,I b) {return si(std::uint32_t(a)-std::uint32_t(b));}
std::uint32_t bits(float value) {return std::bit_cast<std::uint32_t>(value);}
float ordinary(float value) {
    const auto raw=bits(value),exponent=raw&0x7f800000u;
    check(exponent!=0x7f800000u&&(exponent!=0||(raw&0x7fffffu)==0),
          "UI glyph/icon exceptional float");return value;
}
struct Rounding {
    std::fenv_t saved;
    Rounding() {
        check(std::fegetenv(&saved)==0,"UI glyph/icon save fenv");
        if(std::fesetround(FE_TOWARDZERO)) {
            std::fesetenv(&saved);throw std::runtime_error("UI glyph/icon rounding");
        }
    }
    ~Rounding() {std::fesetenv(&saved);}
};
float number(I value) {volatile float out=static_cast<float>(value);return out;}
I integer(float value) {
    ordinary(value);
    check(double(value)>=-2147483648.0&&double(value)<2147483648.0,
          "UI glyph/icon CVT.W.S overflow");
    return static_cast<I>(value); // Reached EE FCSR toward-zero.
}
float arithmetic(float a,float b,char operation) {
    volatile float x=ordinary(a),y=ordinary(b),out;
    check(std::feclearexcept(FE_OVERFLOW|FE_UNDERFLOW)==0,"UI glyph/icon clear exceptions");
    if(operation=='*')out=x*y;else if(operation=='+')out=x+y;else out=x-y;
    check(!(std::fetestexcept(FE_OVERFLOW|FE_UNDERFLOW)&(FE_OVERFLOW|FE_UNDERFLOW)),
          "UI glyph/icon overflow/underflow");return ordinary(out);
}
float mul(float a,float b) {return arithmetic(a,b,'*');}
float plus(float a,float b) {return arithmetic(a,b,'+');}
float minus(float a,float b) {return arithmetic(a,b,'-');}
float scalar(const UiWindow& window,std::size_t at) {
    return ordinary(std::bit_cast<float>(window.word(at)));
}
void context(const UiPlacementContext& c) {
    check(bool(c.random)&&c.sine.size()>=360,"Missing UI placement RNG/sine owner");
}
float random_zero(const UiPlacementContext& c) {
    const auto value=c.random();
    check(value<=0x7fffffffu,"UI placement RNG return outside1f14b0 contract");
    return mul(mul(c.sine[value%360],100.f),0.f);
}
std::array<unsigned,4> components(std::uint32_t color) {
    return {((color>>0)&255)>>2,((color>>8)&255)>>2,
            ((color>>16)&255)>>2,((color>>24)&255)>>2};
}
float packed_color(const std::array<unsigned,4>& c) {
    return number(I(c[0]+(c[1]<<6)+(c[2]<<12)+(c[3]<<18)));
}
}
void UiWindow::place_glyph(unsigned group,unsigned slot,I x,I y,I width,I height,
                           std::span<const Font> fonts,const UiPlacementContext& c) {
    check(group<word(0x3c00)&&slot<16,"UI glyph placement bounds");context(c);
    const std::size_t offset=(std::size_t(group)*16+slot)*16;
    check(offset+16<=glyph_[0].size()&&offset+16<=glyph_[1].size(),"UI glyph storage bounds");
    const auto code=ordinary(std::bit_cast<float>(glyph_[1].word(offset+8)));
    Rounding rounding;
    if(code>32.f) {
        const auto handle=si(word(0x20c));
        check(handle>=0&&std::size_t(handle)<fonts.size(),"UI glyph font handle");
        const auto flags=word(0x4f8);const FontGlyph* glyph=nullptr;
        if((flags&4)&&integer(code)>=256) { //195360: direct metric index.
            const auto index=integer(mul(code,1.f/256.f));
            check(index>=0&&std::size_t(index)<fonts[handle].glyphs.size(),"UI glyph direct font index");
            glyph=&fonts[handle].glyphs[std::size_t(index)];
        } else {
            glyph=fonts[handle].find(std::uint32_t(integer(code)));
            check(glyph!=nullptr,"UI glyph missing font metrics");
        }
        const auto& m=glyph->metrics;
        if(flags&0x80)x=sub(x,width/2); //19c63c..648: signed division truncates.
        else x=integer(minus(number(x),mul(mul(number(std::bit_cast<std::int16_t>(m[2])),
                                              1.f/16.f),scalar(*this,0x224))));
        //19c690..69c: signed metric+2 >>2, but unsigned metric+12 >>2.
        const auto top=I(std::bit_cast<std::int16_t>(m[1]))>>2;
        const auto extent=I(m[9]>>2);
        const auto scale_y=scalar(*this,0x228);
        const auto top_scaled=mul(mul(number(top),scale_y),.5f);
        const auto extent_scaled=mul(mul(number(extent),scale_y),.5f);
        const auto adjusted_y=minus(number(y),top_scaled);
        height=integer(extent_scaled);y=integer(adjusted_y);
    }
    const auto px=number(add(add(x,24),add(si(word(0x23c)),si(word(0x234)))));
    const auto py=plus(plus(minus(number(add(add(y,si(word(0x238))),si(word(0x240)))),
                                 scalar(*this,0x264)),24.f),12.f);
    glyph_[0].put_word(offset,bits(px));glyph_[0].put_word(offset+4,bits(py));
    const auto perturbation=[&]{RngSiteScope site(RngSite::UiGlyph);return random_zero(c);}(); //19c74c: after XY, even for blank/-1.
    glyph_[0].put_word(offset+8,bits(plus(number(c.layer),perturbation)));
    glyph_[0].put_word(offset+12,bits(packed_color(components(word(0x22c))))); //19bed8
    glyph_[1].put_word(offset,bits(number(width)));
    glyph_[1].put_word(offset+4,bits(number(height)));
    glyph_[1].put_word(offset+12,bits(number(si(word(0x20c)))));
}
bool UiWindow::place_icon(unsigned group,unsigned slot,I x,I y,I depth,I selector,
                          const UiPlacementContext& c) {
    check(group<=word(0x3c04)&&slot<16,"UI icon placement bounds");context(c);
    const std::size_t offset=(std::size_t(group)*16+slot)*16;
    check(offset+16<=sprite_[0].size()&&offset+16<=sprite_[1].size(),"UI icon storage bounds");
    check(0x3e40+offset+16<=scalar_bytes,"UI icon embedded color bounds");
    const auto old_selector=ordinary(std::bit_cast<float>(sprite_[1].word(offset+8)));
    Rounding rounding;
    const auto scroll=scalar(*this,0x264);
    const auto px=number(add(x,si(word(0x234))));
    const auto py=minus(number(add(y,si(word(0x238)))),c.layer==1?scroll:mul(scroll,0.f));
    sprite_[0].put_word(offset,bits(px));sprite_[0].put_word(offset+4,bits(py));
    const auto perturbation=[&]{RngSiteScope site(RngSite::UiIcon);return random_zero(c);}(); //19cad4: before depth/color/cache.
    sprite_[0].put_word(offset+8,bits(plus(number(depth),perturbation)));
    const auto color=components(word(0x22c));
    //19cbd4..be8: embedded color stores R,G,A,B, then packed color output.
    for(auto i:{0u,1u,3u,2u})put(0x3e40+offset+i*4,bits(mul(number(I(color[i]<<2)),1.f/128.f)));
    sprite_[0].put_word(offset+12,bits(packed_color(color)));
    if(std::uint32_t(selector)<32) {
        const auto at=std::size_t(selector)*4;
        sprite_[1].put_word(offset,bits(mul(number(si(word(0x10c+at))),scalar(*this,0x224))));
        sprite_[1].put_word(offset+4,bits(mul(number(si(word(0x18c+at))),scalar(*this,0x228))));
        //19cc40 branches to19ccc0: cache+12 is intentionally untouched.
    } else {
        sprite_[1].put_word(offset,bits(plus(scalar(*this,0x280),number(si(word(0x234))))));
        sprite_[1].put_word(offset+4,bits(plus(scalar(*this,0x284),number(si(word(0x238))))));
        const auto second=[&]{RngSiteScope site(RngSite::UiSelect);return random_zero(c);}(); //19cc78: after fallback XY cache stores.
        sprite_[1].put_word(offset+12,bits(plus(scalar(*this,0x288),second)));
    }
    const auto next=number(selector);
    if(old_selector!=next) {sprite_[1].put_word(offset+8,bits(next));return true;}
    return false;
}
}
