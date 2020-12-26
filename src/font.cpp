#include "okage/font.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace okage {
const FontGlyph* Font::find(std::uint32_t code) const {
    return code<first || code>last ? nullptr : &glyphs.at(code-first);
}
Font read_fnt(Bytes b) {
    auto fail=[](){throw std::runtime_error("Unsupported or malformed FNT");};
    if(b.size()<8 || b[0]!=3 || b[4]>b[5])fail();
    auto signed_byte=[&](std::size_t p){return int(__builtin_bit_cast(std::int8_t, (b[p])));};
    auto half=[&](std::size_t p){return unsigned(b[p])|(unsigned(b[p+1])<<8);};
    Font font;font.first=b[4];font.last=b[5];
    font.line_height=(unsigned(b[2])+b[3])*8;
    font.advance_adjustment=signed_byte(6)*16;
    const auto count=std::size_t(font.last-font.first)+1, image_at=8+count*20;
    if(image_at>b.size() || b.size()-image_at<4)fail();
    const unsigned height=half(image_at),width=half(image_at+2);
    if(!width || !height || width>4096 || height>4096 || width%2 ||
       b.size()-image_at-4!=std::size_t(width)*height/2)fail();
    font.atlas={width,height,0x14,0,std::vector<std::uint8_t>(std::size_t(width)*height*4)};
    for(std::size_t i=0;i<count;++i) {
        const auto at=8+i*20;
        const unsigned x0=b[at+8],y0=b[at+9]+half(at+14)*256;
        unsigned x1=b[at+18],y1=b[at+19]+half(at+14)*256;
        if(y1<y0)y1+=256;
        if(x1<x0 || y1<y0 || x1>width || y1>height)fail();
        auto h=[](int value){return static_cast<std::uint16_t>(value);};
        font.glyphs.push_back({{h(half(at)),h(signed_byte(at+2)*8),h(signed_byte(at+4)*16),
            h(signed_byte(at+1)*16),h(x0*16+8),h(y0*16+8),h(x1*16+8),h(y1*16+8),
            h(signed_byte(at)*16),h((signed_byte(at+2)+signed_byte(at+3))*8)}});
    }
    for(std::size_t i=0;i<std::size_t(width)*height;++i) {
        const unsigned index=(b[image_at+4+i/2]>>((i%2)*4))&15;
        auto* p=font.atlas.rgba.data()+i*4;
        // Retail CLUT 0x2003f8: index 0 transparent black; indices 1..15
        // white with GS alpha 128,120,...,16. Normalize GS 128 to RGBA8 255.
        p[0]=p[1]=p[2]=index ? 255 : 0;
        p[3]=index ? std::min(255u,(136-index*8)*2) : 0;
    }
    return font;
}
}
