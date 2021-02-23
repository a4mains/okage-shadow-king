#pragma once
#include "okage/animation.hpp"
#include "okage/camera.hpp"
#include "okage/actor.hpp"
#include "okage/effect.hpp"
#include "okage/collision.hpp"
#include <optional>
namespace okage {
struct ExplorationInput {std::uint16_t held=0;std::uint8_t x=128,z=128;std::uint16_t pressed=0;};
struct FieldNpcDefinition {
    int alias=0;std::string model;std::int32_t state=0,talk=0,alternate_talk=0;
    Float4 position{};float facing=0;
};
struct FieldDefinition {std::uint32_t type=0;std::string event;std::vector<FieldNpcDefinition> npcs;};
FieldDefinition read_field_definition(Bytes database,const std::string& room);
struct PlayerMovement {
    std::uint32_t mask=1,footstep_sound=33,dust_effect=0;
    std::int32_t motion=-1,floor_contact=0;
    std::uint32_t trigger_kind=0,trigger_index=0,camera_area=0,upper_material=0;
    std::uint32_t effect_counter=0,run_delay=0,footstep_clock=0,footstep_period=0;
    std::uint32_t contact_bubble=0,history_cursor=0;
    std::array<Float4,32> history{};
};
class OpeningField {
public:
    OpeningField(Bytes script,std::function<void(int)> start_dialogue,
                 std::function<bool(int)> wait_dialogue,std::span<const ActorBinding> actors={});
    void tick(ExplorationInput input={});
    void load_effects(Bytes);
    void load_exploration_camera(Bytes stg);
    void load_exploration_collision(Bytes xcl,Bytes variables);
    void load_interactions(Bytes database,const std::string& room,Bytes variables);
    const PlayerMovement& player_movement() const {return player_;}
    const std::vector<CollisionBody>& collision_bodies() const {return bodies_;}
    const AttachedEffects& effects() const {return effects_;}
    Float4 position{},target{},rotation{};
    CameraBlend blend;
    std::function<bool(unsigned,std::int32_t)> audio_service;
    std::function<bool(unsigned,std::int32_t,float,unsigned)> sound_cue;
    std::function<void(unsigned,unsigned,std::int32_t)> set_variable;
    std::function<std::int32_t(std::uint32_t)> get_variable;
    int interaction_alias() const {return interaction_alias_;}
    int interaction_event() const {return interaction_event_;}
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
    std::optional<AnchoredCamera> exploration_camera_;
    bool exploration_handoff_=false;
    std::optional<CollisionMap> collision_;
    std::vector<CollisionBody> bodies_;
    PlayerMovement player_;
    std::optional<FieldDefinition> definition_;
    int interaction_alias_=0,interaction_event_=0;
    std::int32_t saved_input_gate_=0;
    void interact(ExplorationInput);
    void move_player(ExplorationInput);
    void remember_player();
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
AnchoredCamera field_anchored_camera(Bytes stg);
}
