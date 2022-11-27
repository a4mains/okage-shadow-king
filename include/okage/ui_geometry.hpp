#pragma once
#include "okage/ui_draw.hpp"
#include "okage/ui_projection.hpp"

namespace okage {
struct UiGeometryTexture {
    enum class Kind { Font, Window } kind;
    std::uint32_t font_index=0;
    std::uint64_t tex0=0;
    std::shared_ptr<const std::vector<Font>> fonts;
    std::shared_ptr<const void> window;
};
struct UiResolvedPrimitive {
    UiDrawReplay source;
    std::uint32_t prim=0; // Source flags OR triangle-fan topology5.
    UiProjectedQuad quad;
    std::optional<UiGeometryTexture> texture;
};
using UiGeometryCommand=std::variant<UiDrawControl,UiDrawRegister,
    UiDrawCurrentFramebuffer,UiGeometryTexture,UiResolvedPrimitive>;
struct UiGeometryList { std::vector<UiGeometryCommand> commands; };
UiGeometryList resolve_ui_geometry(const UiDrawList&);
// Same resolver for independently constructed native command streams/tests.
UiGeometryList resolve_ui_geometry(std::span<const UiDrawCommand>);
}
