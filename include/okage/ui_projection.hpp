#pragma once
#include "okage/shadow.hpp"

namespace okage {
using UiProjectionParameters = ShadowScreenParameters;
struct UiProjectionVertex {
    Float4 position;
    Float4 source_color;
    Float4 auxiliary_color;
    std::array<float,2> uv; // Resolved VU1aa.xy, after UV upload.
};
struct UiProjectedVertex {
    Float4 clip{},screen{};
    std::uint32_t clip_flags=0;
    std::array<float,3> stq{};
    std::array<std::uint32_t,4> xyzf{}; // Raw FTOI4 lanes.
    std::array<std::uint32_t,4> rgba{}; // Raw 2^23-biased color lanes.
};
struct UiProjectedQuad {
    std::array<UiProjectedVertex,4> vertices{};
    bool emitted=false;
};
Float4 ui_projection_color_cache(const Float4& value,
    const Float4& scale={1,1,1,1},const Float4& bias={0,0,0,0});
std::array<float,2> ui_projection_uv_cache(const std::array<float,2>& value,
    const std::array<float,2>& scale={1,1},const std::array<float,2>& bias={0,0});
UiProjectedQuad ui_project_quad(const UiProjectionParameters&,const Matrix4&,
    const std::array<UiProjectionVertex,4>&,bool textured);
}
