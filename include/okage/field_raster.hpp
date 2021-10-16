#pragma once
#include "okage/shadow.hpp"

namespace okage {
struct FieldScreenVertex {
    Float4 clip,screen;
    std::uint32_t flags=0;
};
// Ordinary VU1 entry0xe08: input is already in camera space. This does not
// substitute for entry0xc48's differently ordered matrix/perspective product.
std::vector<FieldScreenVertex> field_screen_vertices(std::span<const Float4>,const ShadowScreenParameters&);
std::array<std::uint32_t,4> field_pack_xyzf(const Float4& screen);
struct FieldRasterVertex {Float4 position,color,uv;};
std::vector<FieldRasterVertex> field_raster_triangles(std::span<const FieldRasterVertex>,
    const ShadowScreenSettings&,bool gs_color_units=false);
}
