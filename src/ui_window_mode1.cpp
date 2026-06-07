#include "okage/ui_window.hpp"
#include "okage/font.hpp"
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::uint32_t bits(float value){return std::bit_cast<std::uint32_t>(value);}
}
void UiWindow::begin_mode1_pass() {
    //19d0e8 entry: a zero cc0 becomes 1, 3e24=cc0-1, 1a58f8, cc0=3e24+1.
    if(word(0xcc0)==0)put(0xcc0,1);
    put(0x3e24,word(0xcc0)-1);
    reset_glyph_packets();
    put(0xcc0,word(0x3e24)+1);
}
void UiWindow::place_mode1_glyph(std::int32_t code,std::int32_t x,std::int32_t y,std::int32_t width,
                                 std::int32_t height,std::span<const Font> fonts,const UiPlacementContext& c) {
    const auto group=word(0x3e24),slot=word(0x3e38);
    check(group<word(0x3c00)&&slot<16,"UI mode1 glyph cursor outside the window's glyph groups");
    //19d5c0: the code lane is written before 19c568 reads it.
    glyph_[1].put_word((std::size_t(group)*16+slot)*16+8,bits(static_cast<float>(code)));
    place_glyph(group,slot,x,y,width,height,fonts,c);
    //19d63c..19d66c: next slot; a full group moves 3e24 to cc0 and grows cc0.
    if(slot+1>15) {put(0x3e38,0);put(0x3e24,word(0xcc0));put(0xcc0,word(0xcc0)+1);}
    else put(0x3e38,slot+1);
}
void UiWindow::clear_mode1_page() {
    reset_glyph_packets();
    put(0x254,word(0x250));
    for(auto at:{0xce8,0xcd0,0x264,0x260,0xcb0,0xcb4,0xcc0,0x3e24,0x3e38})put(at,0);
    const auto groups=word(0x3c00);
    for(unsigned i=0;i<groups*16;++i)glyph_[1].put_word(std::size_t(i)*16+8,bits(-1.f));
}
void UiWindow::finish_mode1_pass(const UiPlacementContext& c) {
    reset_sprite_packets();
    for(unsigned slot=0;slot<16;++slot)(void)place_icon(0,slot,0,0,0,-1,c);
    put(0x1900,word(0x1900)|0x80);
}
}
