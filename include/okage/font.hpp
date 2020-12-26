#pragma once
#include "okage/assets.hpp"
#include <array>

namespace okage {
// Converted retail metrics, in 1/16 pixel units unless stated otherwise.
struct FontGlyph {
    std::array<std::uint16_t,10> metrics{};
};
struct Font {
    std::uint8_t first=0,last=0;
    std::int16_t line_height=0, advance_adjustment=0;
    std::vector<FontGlyph> glyphs;
    Texture atlas;
    const FontGlyph* find(std::uint32_t code) const;
};
Font read_fnt(Bytes bytes);
}
