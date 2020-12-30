#pragma once
#include "okage/pose.hpp"

namespace okage {
using AtanTable=std::array<std::int16_t,4096>;
AtanTable make_atan_table();
float camera_angle(float y,float x,const AtanTable& table);
// Original 0x1b9440 look-at, using the game's quantized turn angles.
Float4 camera_look_at(const Float4& position,const Float4& target,
                      const AtanTable& angles,const SineTable& sine);
// Original 0x1b9690 / 0x1b9590 blending. Position is the evaluated motion
// channel; target is the newly requested look-at point. Both blend together.
struct CameraBlend {
    float phase=1,step=0;
    Float4 position{},target{};
    void begin(float rate,const Float4& from_position,const Float4& from_target);
    void apply(Float4& to_position,Float4& to_target);
};
}
