#pragma once
#include "okage/pose.hpp"
#include <cstdint>
#include <optional>

namespace okage {
struct ShadowTransition {
    std::uint32_t phase=0,locked=0,follow_disabled=0;
    std::int32_t voice=-1;
    float factor=0;
    std::uint16_t hero_shadow=4;
    bool stun_visible=false;
    //0x1b7f88 writes a mode4 shadow-projection parameter, not HERO opacity.
    float projection_factor=1;
    Float4 stun_rotation{},stun_position{},stun_scale{1,1,1,1};
    void tick(const Float4& hero_rotation,const Float4& hero_position);
};
struct ShadowProjection {
    std::array<std::uint32_t,3> color{};
    bool has_matrices=false;
    std::array<Matrix4,2> matrices{};
    std::optional<Float4> direction;
};
struct ShadowProjectionContext {
    float slope=0,span=0;
    Matrix4 view{},inverse{};
    std::array<Matrix4,2> planes{};
    Float4 world_direction{},view_direction{};
    ShadowProjectionContext(const Float4& light_direction,const Matrix4& camera_view);
    ShadowProjection project(unsigned mode,float height,float factor,
        const std::array<std::uint32_t,3>& tint={255,64,192}) const;
};
struct ShadowPrismInput {
    std::array<Float4,6> camera{},screen{}; // Lower triangle, then upper triangle.
    std::array<std::uint32_t,6> clip{};
    Float4 direction{};
    std::int32_t clip_status=0;
};
struct ShadowPrism {
    bool emitted=false;
    std::array<std::array<std::array<std::uint32_t,4>,12>,3> passes{};
};
ShadowPrism shadow_prism(const ShadowPrismInput& input);
}
