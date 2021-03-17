#pragma once
#include "okage/pose.hpp"

namespace okage {
struct CollisionMap;
using AtanTable=std::array<std::int16_t,4096>;
AtanTable make_atan_table();
float camera_angle(float y,float x,const AtanTable& table);
// Original 0x1b9440 look-at, using the game's quantized turn angles.
Float4 camera_look_at(const Float4& position,const Float4& target,
                      const AtanTable& angles,const SineTable& sine);
struct AnchoredCamera {
    Float4 anchor{};
    float distance_offset=0,distance_scale=0,target_height=0;
};
struct CameraPoints {Float4 position{},target{};};
// Native exterior mode0 (0x1363b8/0x136c10/0x136d00). These values belong to
// the camera manager and survive scene initialization unless begin resets them.
struct ExteriorCamera {
    std::int32_t preset=0,obstruction_clock=0,distance=100;
    float distance_scale=0.976f,target_height=-6,height=-2.2f;
    float turn_rate=.0003f,saved_scale=0.976f,desired_height=0,maximum_height=0,phase=0;
    Float4 previous_player{};
    float angular_velocity=0,target_y=0,target_offset=0;
    bool manual_rotate=false,fast_turn=false,snap_target=true;
    std::int32_t target_reset_counter=0,target_reset_clock=0;
    Float4 begin(const Float4& player);
    void configure(float height,float scale,float target);
    void select_preset(unsigned value);
    CameraPoints tick(const Float4& player,const Float4& facing,const Float4& camera,
                      unsigned held,unsigned look_x,const CollisionMap&,
                      const AtanTable&,const SineTable&);
};
// Original room-camera manager mode 1, 0x136aa8.
CameraPoints anchored_camera(const Float4& player,const AnchoredCamera& settings,
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
