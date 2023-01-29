#pragma once
#include "okage/camera.hpp"
#include "okage/assets.hpp"
#include <optional>
#include <functional>
#include <vector>

namespace okage {
class Animation;
struct BattleCameraPreset {
    unsigned position_mode=0,target_mode=0;
    float position_duration=0,target_duration=0;
    Float4 position_start{},position_end{},target_start{},target_end{};
    float projection_duration=0,projection_value=0;
};
// Authored 0x90-byte preset: three 0x30-byte records. Modes beyond the
// reached fixed/linear unanchored paths remain explicit guards.
BattleCameraPreset read_battle_camera_preset(Bytes data);
using BattleCameraSequence=std::array<std::optional<BattleCameraPreset>,3>;
class BattleCameraMotion {
public:
    BattleCameraMotion(const BattleCameraSequence&,float timestep);
    void restart_sequence(const BattleCameraSequence&);
    void tick();
    void tick(Animation& channels,const std::function<void(float)>& projection);
    Float4 position() const {return position_.point;}
    Float4 target() const {return target_.point;}
    Float4 rotation() const {return position_.auxiliary;}
    bool ready() const {return sequence_phase_==3;}
    // One-time projection request corresponding to 0x14f070, to be applied by
    // the owning battle renderer. Motion probes leave this separate sink pending.
    std::optional<float> take_projection_request();
    // Semantic state in the original body+0x10..0xeb order, plus sequence phase;
    // excludes guest pointers/channels. For bounded differential diagnostics.
    std::array<std::uint32_t,56> observation() const;
private:
    struct Lane {
        unsigned mode=0;
        float remaining=0;
        Float4 point{},auxiliary{},step{},reserved{},endpoint{};
    };
    BattleCameraSequence sequence_;
    float timestep_;
    AtanTable angles_;
    Lane position_,target_;
    float projection_duration_=0,projection_value_=0;
    unsigned projection_dirty_=0,flags_=0,position_phase_=0,sequence_phase_=0;
    void select(const BattleCameraPreset&);
    void aim();
    void advance(Animation* channels,const std::function<void(float)>* projection);
};
}
