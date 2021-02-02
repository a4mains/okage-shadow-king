#pragma once
#include "okage/animation.hpp"
#include "okage/camera.hpp"
#include "okage/actor.hpp"
#include "okage/effect.hpp"
namespace okage {
class OpeningField {
public:
    OpeningField(Bytes script,std::function<void(int)> start_dialogue,
                 std::function<bool(int)> wait_dialogue,std::span<const ActorBinding> actors={});
    void tick();
    void load_effects(Bytes);
    const AttachedEffects& effects() const {return effects_;}
    Float4 position{},target{},rotation{};
    CameraBlend blend;
    std::function<bool(unsigned,std::int32_t)> audio_service;
    std::function<bool(unsigned,std::int32_t,float,unsigned)> sound_cue;
    std::function<void(unsigned,unsigned,std::int32_t)> set_variable;
    unsigned pending_opcode=0;
    int pending_actor=0,pending_animation=0;
    std::uint32_t pc() const {return script_.pc();}
    bool event_running() const {return (script_.script_state()&0x100)!=0;}
    unsigned camera_mode() const {return camera_mode_;}
    Matrix4 view() const;
private:
    std::vector<std::uint8_t> bytes_;
    Animation script_;
    SineTable sine_=make_sine_table();
    AtanTable angles_=make_atan_table();
    Float4 requested_target_{};
    std::vector<ActorBinding> actors_;
    FieldActor* actor(int alias) const;
    int group_=-2;
    unsigned camera_mode_=2;
    std::vector<EffectDefinition> effect_definitions_;
    AttachedEffects effects_;
    std::array<unsigned,16> effect_ring_{};
    unsigned effect_ring_index_=0;
    std::function<void(int)> start_dialogue_;
    std::function<bool(int)> wait_dialogue_;
    Animation::ServiceResult service(unsigned,unsigned,std::int32_t,std::uint32_t);
    std::uint32_t word(std::size_t) const;
    Float4 point(std::size_t) const;
    void evaluate();
};
// Clip-space projection coefficients x/y/z scale and z offset. Uses the STG
// projection record and recovered 640x224 field near/far defaults (3,1024).
Float4 field_projection(Bytes stg);
}
