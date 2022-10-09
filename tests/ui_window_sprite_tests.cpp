#include "okage/ui_window.hpp"
#include "okage/ui_layout_geometry.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace okage {
struct UiWindowSpriteTestAccess {
    static void origin(UiWindow& w,std::uint32_t x,std::uint32_t y) {w.put(0x234,x);w.put(0x238,y);}
    static void unknown_cache(UiWindow& w) {w.sprite_[1].known_[0]=false;}
    static void exceptional_cache(UiWindow& w) {w.sprite_[1].put_word(0,0x7fc00001);}
};
}
namespace {
using namespace okage;
void require(bool b,const char* s) {if(!b)throw std::runtime_error(s);}
template<class F>void rejects(F f) {
    try {f();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected sprite rejection");
}
std::uint32_t bits(float f) {return __builtin_bit_cast(std::uint32_t, (f));}
UiWindow window() {
    return UiWindow(2,2,0,std::make_shared<UiWindowStream>(UiWindowStream{1,{}}));
}
UiLayoutSprite operation() {
    return {10,-20,16,8,32,9,77,4,{1,2,{0xff804307,UiLayoutColorConversion::Direct}}};
}
struct Snapshot {
    std::vector<std::optional<std::uint32_t>> scalar;
    std::vector<std::optional<std::uint8_t>> data;
    bool operator==(const Snapshot&)const=default;
};
Snapshot snapshot(const UiWindow& w) {
    Snapshot s;
    for(std::size_t at=0;at<UiWindow::scalar_bytes;at+=4)s.scalar.push_back(w.known_word(at));
    for(unsigned i=0;i<2;++i)for(std::size_t at=0;at<w.sprite_buffer(i).size();++at)
        s.data.push_back(w.sprite_buffer(i).known_byte(at));
    return s;
}
void stores_and_dirty() {
    auto w=window();auto op=operation();
    UiWindowSpriteTestAccess::origin(w,3,std::uint32_t(-4));
    require(w.place_sprite(1,15,op),"First placement changes cache");
    constexpr unsigned at=31*16;
    const auto& p=w.sprite_buffer(0);const auto& c=w.sprite_buffer(1);
    require(p.word(at)==bits(13)&&p.word(at+4)==bits(-24),"Origin addition");
    require(p.word(at+8)==bits(1048584.f),"Dimensions numerically converted");
    require(p.word(at+12)==bits(16647169.f),"Packed six-bit RGBA numerically converted");
    require(c.word(at)==bits(1)&&c.word(at+4)==bits(2)&&c.word(at+8)==bits(34)&&
            c.word(at+12)==bits(2097161.f),"Scale/kind/UV cache");
    require(w.word(0x3e40+at)==bits(.03125f)&&w.word(0x3e44+at)==bits(.5f)&&
            w.word(0x3e48+at)==bits(1)&&w.word(0x3e4c+at)==bits(1.96875f),"Six-bit normalized color");
    require(w.word(0x224)==bits(1)&&w.word(0x228)==bits(2)&&w.word(0x22c)==op.style.color.rgba,
            "Explicit caller style setup");
    const auto before=snapshot(w);
    require(!w.place_sprite(1,15,op)&&snapshot(w)==before,"Unchanged placement is clean");
    op.x=99;op.height=17;op.style.color.rgba=0x01020304;
    require(!w.place_sprite(1,15,op),"Position/dimension/color writes do not dirty cache");
    require(p.word(at)==bits(102)&&p.word(at+8)==bits(1048593.f)&&p.word(at+12)==bits(1),
            "Clean placement still writes data");
    op.style.scale_y=3;require(w.place_sprite(1,15,op),"Scale change dirty");
    op.v=10;require(w.place_sprite(1,15,op),"UV change dirty");
    require(p.word(at-16)==0&&c.word(at-8)==bits(-1)&&p.word(at+16)==0&&c.word(at+16)==0,
            "Adjacent slots and sentinel group preserved");
    require(!w.sprite_cache(1,0).known_byte(0)&&!w.sprite_cache(1,1).known_byte(0)&&
            !w.glyph_buffer(0).known_byte(0)&&!w.known_word(0x658),
            "No invented packets, glyph bytes or pointers");
    require(w.word(0x3e40+at-16)==0&&w.word(0x3e40+at+16)==0,"Adjacent embedded color lanes preserved");
}
void numeric_and_wrapping() {
    auto w=window();auto op=operation();
    const int previous=std::fegetround();std::fesetround(FE_UPWARD);
    op.x=16777219;op.y=-16777219;op.width=256;op.height=3;op.u=256;op.v=5;
    require(w.place_sprite(0,0,op),"Numeric fixture first placement");
    require(w.sprite_buffer(0).word(0)==bits(16777218.f)&&
            w.sprite_buffer(0).word(4)==bits(-16777218.f)&&
            w.sprite_buffer(0).word(8)==bits(16777218.f)&&
            w.sprite_buffer(1).word(12)==bits(16777220.f),"CVT toward zero, not nearest/bitcast");
    require(std::fegetround()==FE_UPWARD,"Host rounding restored");std::fesetround(previous);
    op.width=1;op.height=-1;op.u=1;op.v=-1;
    w.place_sprite(0,0,op);
    require(w.sprite_buffer(0).word(8)==bits(65535)&&w.sprite_buffer(1).word(12)==bits(65535),
            "Signed low operand borrows from high operand");
    op.width=-32768;op.height=-1;op.u=-32768;op.v=-1;
    op.x=std::numeric_limits<std::int32_t>::max();op.y=std::numeric_limits<std::int32_t>::min();
    UiWindowSpriteTestAccess::origin(w,1,0xffffffff);
    w.place_sprite(0,0,op);
    require(w.sprite_buffer(0).word(0)==0xcf000000&&w.sprite_buffer(0).word(4)==0x4effffff&&
            w.sprite_buffer(0).word(8)==0x4effffff&&w.sprite_buffer(1).word(12)==0x4effffff,
            "Position/dimension/UV wrap32 before CVT");
    op.style.scale_x=-0.f;op.style.scale_y=0.f;w.place_sprite(0,0,op);
    require(w.sprite_buffer(1).word(0)==0x80000000,"Changed scale writes negative zero");
    op.style.scale_x=0.f;op.style.scale_y=-0.f;
    require(!w.place_sprite(0,0,op)&&w.sprite_buffer(1).word(0)==0x80000000&&
            w.sprite_buffer(1).word(4)==0,"Equal zero comparison preserves cached signs");
    require(w.word(0x224)==0&&w.word(0x228)==0x80000000,"Caller style keeps new zero signs");
}
void colors_and_guards() {
    auto w=window();auto op=operation();
    op.style.color={0xfff8f8f8,UiLayoutColorConversion::Convert19bdf8};
    w.place_sprite(0,0,op);
    require(w.word(0x22c)==0xfe808080&&w.word(0x3e40)==bits(1)&&
            w.word(0x3e4c)==bits(1.96875f),"Source conversion before six-bit placement quantization");
    const auto before=snapshot(w);
    rejects([&]{w.place_sprite(2,0,op);});rejects([&]{w.place_sprite(0,16,op);});
    rejects([&]{w.place_sprite(~0u,~0u,op);});
    for(auto invalid:{0x7f800000u,0xff800000u,0x7fc00001u,1u,0x80000001u}) {
        op.style.scale_x=__builtin_bit_cast(float, (invalid));
        rejects([&]{w.place_sprite(0,0,op);});
    }
    op.style.scale_x=1;op.style.color.conversion=static_cast<UiLayoutColorConversion>(99);
    rejects([&]{w.place_sprite(0,0,op);});
    require(snapshot(w)==before,"Invalid request rejected before any mutation");
    op=operation();
    UiWindowSpriteTestAccess::unknown_cache(w);
    const auto unknown=snapshot(w);
    rejects([&]{w.place_sprite(0,0,op);});
    require(snapshot(w)==unknown,"Unknown comparison bytes not silently initialized");
    UiWindowSpriteTestAccess::exceptional_cache(w);
    const auto exceptional=snapshot(w);
    rejects([&]{w.place_sprite(0,0,op);});
    require(snapshot(w)==exceptional,"Exceptional comparison operand rejected before mutation");
}
}
int main() {
    try {stores_and_dirty();numeric_and_wrapping();colors_and_guards();
        std::cout<<"UI window sprite tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
