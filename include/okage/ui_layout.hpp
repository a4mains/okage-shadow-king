#pragma once
#include "okage/ui_window.hpp"
#include <optional>

namespace okage {
struct UiPacketBuildContext;
struct UiLayoutShared {
    std::uint32_t sprite_groups=0; //200520
    std::int32_t layer=0; //20052c
    bool sprite_dirty=false; //200530
    std::optional<std::array<std::uint32_t,2>> glyph_metric; //2006ac
};
struct UiLayoutContext {
    UiLayoutShared& shared;
    std::span<const Font> fonts;
    std::span<const float> sine;
    std::function<std::uint32_t()> random;
    std::int32_t resource_y_offset; // Actual selected resource's200a88 value.
    const UiPacketBuildContext& packets;
};
}
