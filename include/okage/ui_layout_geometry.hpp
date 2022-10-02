#pragma once
#include "okage/ruw.hpp"
#include <cstdint>
#include <vector>

namespace okage {
enum class UiLayoutColorConversion { Direct, Convert19bdf8 };
struct UiLayoutColor {
    std::uint32_t rgba=0x80808080;
    UiLayoutColorConversion conversion=UiLayoutColorConversion::Direct;
    bool operator==(const UiLayoutColor&) const = default;
};
// Resolve Direct or source19bdf8 without discarding input provenance.
std::uint32_t ui_layout_color_rgba(UiLayoutColor);
struct UiLayoutSpriteStyle {
    float scale_x=1,scale_y=1; // Window224/228 at each19c828 invocation.
    UiLayoutColor color;
};
struct UiLayoutSprite {
    std::int32_t x,y,width,height,u,v;
    std::int32_t texture_index;
    unsigned piece;
    UiLayoutSpriteStyle style;
};
struct UiLayoutSpriteBatch {
    std::vector<UiLayoutSprite> sprites;
    UiLayoutSpriteStyle final_style;
};
UiLayoutSpriteBatch layout_ruw_sprites(const RuwResource&,const RuwWidget&,
    std::int32_t y_offset,UiLayoutSpriteStyle incoming={});
}
