#pragma once
#include "okage/shadow.hpp"

namespace okage {
struct FieldProjectionFrame {
    ShadowScreenSettings shadow{},ordinary{},field_end{},effects{},deferred{},ending{};
};
class FieldProjection {
public:
    void reset_scene(); //0x1b8028, preserving display scale/viewport.
    void load_stage(Bytes);
    void apply_stage_setting(unsigned,const Float4&,float depth_min,float depth_max);
    void synchronize_stage_effects();
    void submit_empty_resource_frame(bool effect_pass,float depth_min,float depth_max);
    std::array<float,4> effect_parameters() const {
        return {effect_near_,effect_far_,effect_fog_near_,effect_fog_far_};
    }
    void begin_field();
    //1b9be0 consumes the live337e30/34 range after copying shadow settings.
    void begin_field(float depth_min,float depth_max);
    void begin_effects();
    void select_effect_near(float);
    void begin_deferred();
    void end_effects();
    const FieldProjectionFrame& frame() const {return frame_;}
    const ShadowScreenSettings& current() const {return current_;}
    float effect_near() const {return effect_near_;}
    std::uint64_t frames() const {return frames_;}
private:
    ShadowScreenSettings current_{1024,1,1,2048,2048,101,16777115,
        3,1024,3,1024,4096,16384,320,112};
    FieldProjectionFrame frame_{};
    float field_far_=1024,field_fog_near_=4096,field_fog_far_=16384,effect_near_=3;
    float effect_far_=1024,effect_fog_near_=4096,effect_fog_far_=16384;
    bool loaded_=false;
    enum class Phase {Idle,Field,Effects,Deferred};
    Phase phase_=Phase::Idle;
    std::uint64_t frames_=0;
};
Float4 field_clip_projection(const ShadowScreenSettings&);
// Stable diagnostic order, matching the original projection-global observer.
std::array<float,15> field_projection_words(const ShadowScreenSettings&);
}
