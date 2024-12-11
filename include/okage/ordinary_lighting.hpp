#pragma once
#include "okage/pose.hpp"

namespace okage {
// First extra material pass: 0x183ff0/0x1ae290 and VU1 entry0x116.
// Region stores authored offsetX/Y and extentX/Y in 1/1024 texture units.
std::array<float,2> ordinary_environment_uv(const Float4& normalized_normal,const Float4& region);
Float4 ordinary_environment_source(float coefficient);
enum class OrdinaryNormalOperation { direct, overwrite, accumulate };
enum class OrdinaryLightingPath { direct, deferred };

Float4 ordinary_transform_normal(const Matrix4& matrix, const Float4& source,
    OrdinaryNormalOperation operation, const Float4& initial_cache = {});

struct OrdinaryLightingResult {
    Float4 normalized_normal;
    Float4 lighting_cache;
};
OrdinaryLightingResult ordinary_light_normal(const Float4& normal_cache,
    const Matrix4& direction_matrix, const Matrix4& light_colors,
    OrdinaryLightingPath path);

Float4 ordinary_material_modulation(const Float4& source_cache, const Float4& lighting_cache);
Float4 ordinary_material_color(const Float4& source_cache, const Float4& lighting_cache);

Matrix4 ordinary_direction_matrix(const std::array<Float4, 3>& stg_directions,
    const Matrix4& view);

struct OrdinaryNormalCache {
    OrdinaryLightingPath path;
    std::vector<Float4> normals; // Before normalization/lighting.
    std::vector<bool> defined;
};
OrdinaryNormalCache ordinary_normal_cache(const RenderBlock& block,
    std::span<const Matrix4> view_world_pose);

}
