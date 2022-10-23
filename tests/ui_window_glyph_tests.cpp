#include "okage/ui_window.hpp"
#include "okage/font.hpp"
#include <bit>
#include <cfenv>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace okage {
struct UiWindowGlyphTestAccess {
    static void set(UiWindow& w,std::size_t at,std::uint32_t value) {w.put(at,value);}
    static void code(UiWindow& w,unsigned group,unsigned slot,float value) {
        w.glyph_[1].put_word((group*16+slot)*16+8,__builtin_bit_cast(std::uint32_t, (value)));
    }
    static void unknown_code(UiWindow& w) {w.glyph_[1].known_[8]=false;}
    static void sprite_word(UiWindow& w,unsigned at,std::uint32_t value) {w.sprite_[1].put_word(at,value);}
};
}
namespace {
using namespace okage;
void require(bool b,const char* message) {if(!b)throw std::runtime_error(message);}
template<class F>void rejects(F f) {
    try {f();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected glyph/icon rejection");
}
std::uint32_t bits(float f) {return __builtin_bit_cast(std::uint32_t, (f));}
void set(UiWindow& w,std::size_t at,std::uint32_t value) {UiWindowGlyphTestAccess::set(w,at,value);}
UiWindow window() {
    return UiWindow(2,2,0,std::make_shared<UiWindowStream>(UiWindowStream{1,{}}));
}
std::array<Font,1> fonts() {
    Font f;f.first=65;f.last=66;f.glyphs.resize(2);
    f.glyphs[0].metrics[1]=std::uint16_t(-13);f.glyphs[0].metrics[2]=std::uint16_t(-25);
    f.glyphs[0].metrics[9]=0xfff8;
    f.glyphs[1].metrics[1]=8;f.glyphs[1].metrics[2]=16;f.glyphs[1].metrics[9]=16;
    return {f};
}
struct Random {
    unsigned draws=0;std::uint32_t state=123;
    std::uint32_t next() {++draws;state=state*0x41c64e6du+0x3039;return state&0x7fffffffu;}
};
std::array<float,360> sine() {
    std::array<float,360> s{};for(unsigned i=0;i<s.size();++i)s[i]=(i%2)?-.25f:.5f;return s;
}
void glyph_metrics_and_order() {
    auto w=window();auto f=fonts();auto s=sine();Random random;
    set(w,0x234,5);set(w,0x238,std::uint32_t(-2));set(w,0x23c,3);set(w,0x240,4);
    set(w,0x264,bits(.25f));set(w,0x224,bits(1.5f));set(w,0x228,bits(.75f));
    set(w,0x22c,0xff804307);UiWindowGlyphTestAccess::code(w,1,15,65);
    constexpr unsigned at=31*16;
    UiPlacementContext context{1,s,[&]{
        require(w.glyph_buffer(0).word(at)==bits(44)&&w.glyph_buffer(0).word(at+4)==bits(58.75f),
                "Glyph XY stores precede RNG");
        require(!w.glyph_buffer(0).known_byte(at+8)&&!w.glyph_buffer(1).known_byte(at),
                "Glyph depth/dimensions not written before RNG");return random.next();}};
    const int saved=std::fegetround();std::fesetround(FE_UPWARD);
    w.place_glyph(1,15,10,20,13,777,f,context);
    require(std::fegetround()==FE_UPWARD,"Placement restores host rounding");std::fesetround(saved);
    require(random.draws==1,"One glyph RNG draw");
    require(w.glyph_buffer(0).word(at+8)==bits(1)&&w.glyph_buffer(0).word(at+12)==bits(16647169.f),
            "Layer/color conversion");
    require(w.glyph_buffer(1).word(at)==bits(13)&&w.glyph_buffer(1).word(at+4)==bits(6143)&&
            w.glyph_buffer(1).word(at+8)==bits(65)&&w.glyph_buffer(1).word(at+12)==bits(0),
            "Signed top/bearing, unsigned height, preserved code and font");
    require(!w.glyph_buffer(0).known_byte(at-1)&&!w.glyph_buffer(0).known_byte(at+16)&&
            !w.glyph_cache(1,0).known_byte(0)&&!w.known_word(0x650),"Unknown adjacent/cache/pointer bytes preserved");
    context.random=[&]{return random.next();};
    w.place_glyph(1,15,10,20,13,777,f,context);
    require(random.draws==2,"Unchanged glyph still draws");
    set(w,0x4f8,0x80);w.place_glyph(1,15,10,20,5,777,f,context);
    require(w.glyph_buffer(0).word(at)==bits(40),"Centered width divides signed integer before subtract");
    w.place_glyph(1,15,10,20,-5,777,f,context);
    require(w.glyph_buffer(0).word(at)==bits(44),"Negative odd center width truncates toward zero");
    set(w,0x224,0x7fc00000);w.place_glyph(1,15,10,20,5,777,f,context);
    require(w.glyph_buffer(0).word(at)==bits(40),"Centered branch never reads X scale");
    set(w,0x224,bits(1.5f));
    set(w,0x4f8,4);UiWindowGlyphTestAccess::code(w,1,15,256);
    w.place_glyph(1,15,10,20,13,777,f,context);
    require(w.glyph_buffer(0).word(at)==bits(40)&&w.glyph_buffer(0).word(at+4)==bits(56.75f)&&
            w.glyph_buffer(1).word(at+4)==bits(1),"Bit4 encoded index uses direct metric record");
}
void glyph_blank_and_numeric() {
    auto w=window();auto s=sine();Random random;UiPlacementContext c{-7,s,[&]{return random.next();}};
    set(w,0x20c,0xffffffff);set(w,0x23c,0);set(w,0x240,0);
    for(float code:{-1.f,0.f,32.f}) {
        UiWindowGlyphTestAccess::code(w,0,0,code);
        w.place_glyph(0,0,10,20,-5,17,{},c);
        require(w.glyph_buffer(0).word(0)==bits(34)&&w.glyph_buffer(0).word(4)==bits(56)&&
                w.glyph_buffer(0).word(8)==bits(-7)&&w.glyph_buffer(1).word(4)==bits(17)&&
                w.glyph_buffer(1).word(8)==bits(code)&&w.glyph_buffer(1).word(12)==bits(-1),
                "Blank/sentinel skips metrics but writes all placement data");
    }
    require(random.draws==3,"Blank/sentinel draw count");
    w.place_glyph(0,0,16777195,-16777219,16777219,-16777219,{},c);
    require(w.glyph_buffer(0).word(0)==bits(16777218.f)&&w.glyph_buffer(1).word(0)==bits(16777218.f)&&
            w.glyph_buffer(1).word(4)==bits(-16777218.f),"Signed CVT.S.W toward zero");
    w.place_glyph(0,0,0,268435456,0,0,{},c);
    require(w.glyph_buffer(0).word(4)==bits(268435456.f),"Y adds24 then12 separately, not folded36");
    set(w,0x234,1);w.place_glyph(0,0,std::numeric_limits<std::int32_t>::max(),0,0,0,{},c);
    require(w.glyph_buffer(0).word(0)==0xceffffff,"Glyph X wrapping precedes numeric conversion");
    set(w,0x234,0);c.layer=0;c.random=[&]{++random.draws;return 1u;};
    w.place_glyph(0,0,0,0,0,0,{},c);
    require(w.glyph_buffer(0).word(8)==0,"+0 layer plus negative random zero preserves source addition");
}
void icon_branches_and_order() {
    auto w=window();auto s=sine();Random random;
    set(w,0x234,3);set(w,0x238,4);set(w,0x264,bits(2.5f));
    set(w,0x224,bits(1.5f));set(w,0x228,bits(.25f));set(w,0x10c+7*4,11);set(w,0x18c+7*4,13);
    UiWindowGlyphTestAccess::sprite_word(w,12,0x3f123456);
    UiPlacementContext c{1,s,[&]{require(w.sprite_buffer(0).word(0)==bits(13)&&
        w.sprite_buffer(0).word(4)==bits(21.5f),"Icon XY before first RNG");return random.next();}};
    require(w.place_icon(0,0,10,20,9,7,c),"Icon selector change dirty");
    require(random.draws==1&&w.sprite_buffer(0).word(8)==bits(9)&&
            w.sprite_buffer(1).word(0)==bits(16.5f)&&w.sprite_buffer(1).word(4)==bits(3.25f)&&
            w.sprite_buffer(1).word(8)==bits(7)&&w.sprite_buffer(1).word(12)==0x3f123456,
            "Atlas scaling and untouched fourth cache lane");
    c.random=[&]{return random.next();};c.layer=0;set(w,0x224,bits(2));
    require(!w.place_icon(0,0,30,40,5,7,c)&&random.draws==2&&
            w.sprite_buffer(0).word(4)==bits(44)&&w.sprite_buffer(1).word(0)==bits(22),
            "Only selector controls dirty; unchanged icons still draw and update dimensions");
    set(w,0x280,bits(10.5f));set(w,0x284,bits(20.25f));set(w,0x288,bits(-0.f));
    unsigned calls=0;c.random=[&]{
        ++calls;
        if(calls==2)require(w.sprite_buffer(1).word(0)==bits(13.5f)&&
                           w.sprite_buffer(1).word(4)==bits(24.25f)&&
                           w.sprite_buffer(1).word(8)==bits(7),"Fallback XY before second RNG; selector after it");
        ++random.draws;return 1u;};
    require(w.place_icon(0,0,0,0,0,-1,c)&&calls==2&&w.sprite_buffer(1).word(12)==0x80000000,
            "Sentinel consumes two draws and preserves signed-zero arithmetic");
    c.random=[&]{++random.draws;return 0u;};const auto before=random.draws;
    require(!w.place_icon(0,0,0,0,0,-1,c)&&random.draws==before+2,"Unchanged sentinel still two draws");
    set(w,0x224,0x7fc00000);set(w,0x228,0x7fc00000);
    const auto count=random.draws;
    require(w.place_icon(0,0,0,0,0,32,c)&&random.draws==count+2,"Selector32 takes fallback branch");
    require(w.word(0x3e40)==bits(1)&&w.word(0x3e4c)==bits(1)&&!w.sprite_cache(0,0).known_byte(0),
            "Icon embedded RGBA written without fabricated packets");
}
void guards() {
    auto w=window();auto f=fonts();auto s=sine();Random random;UiPlacementContext c{1,s,[&]{return random.next();}};
    rejects([&]{w.place_glyph(2,0,0,0,1,1,f,c);});rejects([&]{w.place_glyph(0,16,0,0,1,1,f,c);});
    rejects([&]{w.place_icon(3,0,0,0,1,1,c);});rejects([&]{w.place_icon(0,16,0,0,1,1,c);});
    auto missing=c;missing.random={};rejects([&]{w.place_glyph(0,0,0,0,1,1,f,missing);});
    missing=c;missing.sine=std::span<const float>(s).first(359);rejects([&]{w.place_icon(0,0,0,0,1,1,missing);});
    UiWindowGlyphTestAccess::code(w,0,0,67);rejects([&]{w.place_glyph(0,0,0,0,1,1,f,c);});
    UiWindowGlyphTestAccess::unknown_code(w);rejects([&]{w.place_glyph(0,0,0,0,1,1,f,c);});
    for(auto raw:{0x7fc00000u,1u}) {
        UiWindowGlyphTestAccess::code(w,0,0,__builtin_bit_cast(float, (raw)));
        rejects([&]{w.place_glyph(0,0,0,0,1,1,f,c);});
    }
    require(random.draws==0&&!w.glyph_buffer(0).known_byte(0),"Invalid glyph requests neither place nor consume RNG");
    UiWindowGlyphTestAccess::code(w,0,0,65);set(w,0x228,bits(std::numeric_limits<float>::max()));
    rejects([&]{w.place_glyph(0,0,0,0,1,1,f,c);});
    require(random.draws==0,"Metric overflow before RNG");
}
void exact_capacity_tail() {
    auto w=window();auto s=sine();Random random;UiPlacementContext c{1,s,[&]{return random.next();}};
    w.place_icon(1,15,10,20,0,7,c);
    for(unsigned slot=0;slot<16;++slot)require(w.place_icon(2,slot,0,0,0,-1,c),
        "Extra sentinel group initially has cleared selector0, not initialized-1");
    require(random.draws==33,"Exact capacity wrap runs actual two-draw tail placements");
    require(w.sprite_buffer(1).word(47*16+8)==bits(-1)&&
            w.word(0x3e40+47*16)==bits(1),"Last sentinel cache/color lanes within real allocations");
    require(!w.sprite_cache(1,0).known_byte(0),"Tail does not fabricate packet caches");
    rejects([&]{w.sprite_cache(2,0);});
}
void owned_font(const char* path) {
    std::ifstream in(path,std::ios::binary);require(bool(in),"Owned font input");
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};
    const std::array<Font,1> f{read_fnt(bytes)};auto w=window();auto s=sine();Random random;
    UiPlacementContext c{1,s,[&]{return random.next();}};
    unsigned placed=0;
    for(unsigned code=f[0].first;code<=f[0].last;++code) {
        UiWindowGlyphTestAccess::code(w,0,0,float(code));
        w.place_glyph(0,0,10,20,12,24,f,c);++placed;
    }
    require(random.draws==placed,"Owned font all glyph placement draws");
    std::cout<<"Owned font glyphs placed="<<placed<<'\n';
}
}
int main(int argc,char** argv) {
    try {glyph_metrics_and_order();glyph_blank_and_numeric();icon_branches_and_order();guards();exact_capacity_tail();
        for(int i=1;i<argc;++i)owned_font(argv[i]);
        std::cout<<"UI window glyph/icon tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
