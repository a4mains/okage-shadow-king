#pragma once
#include "okage/pose.hpp"
#include <cstdint>

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
}
