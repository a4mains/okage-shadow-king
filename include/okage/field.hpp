#pragma once
#include "okage/animation.hpp"
#include "okage/camera.hpp"
#include "okage/actor.hpp"
#include "okage/effect.hpp"
#include "okage/collision.hpp"
#include "okage/enemy.hpp"
#include "okage/screen_filter.hpp"
#include "okage/shadow.hpp"
#include "okage/field_projection.hpp"
#include <optional>
namespace okage {
class MapObjects;
struct ExplorationInput {std::uint16_t held=0;std::uint8_t x=128,z=128;std::uint16_t pressed=0;std::uint8_t look_x=128;};
struct FieldNpcDefinition {
    int alias=0;std::string model;std::int32_t state=0,talk=0,alternate_talk=0;
    Float4 position{};float facing=0;unsigned flags=1;
};
struct FieldDefinition {
    int scene=-1;std::uint32_t type=0,party=0,startup=0,field_flags=0;
    std::int32_t music=0,secondary_music=0;
    unsigned footstep=33,dust=0;
    std::string room_directory,room,message,event;
    std::vector<FieldNpcDefinition> npcs;
    std::vector<unsigned> followers;
};
FieldDefinition read_field_definition(Bytes database,const std::string& room);
FieldDefinition read_field_definition(Bytes database,int scene);
// 0x131d28 defaults followed by synchronous entry zero. This is field setup,
// not cold game initialization; inputs must already contain scene/entrance/gate.
std::array<std::uint8_t,0x800> field_setup_state(Bytes script,Bytes variables,const FieldDefinition& definition);
FieldDefinition prepare_field_definition(Bytes script,Bytes variables,FieldDefinition definition);
FieldDefinition prepare_field_entry(Bytes script,std::span<std::uint8_t> variables,
                                    FieldDefinition definition,unsigned entrance);
struct FieldEntrance {Float4 position{},rotation{};};
FieldEntrance field_entrance(Bytes stg,unsigned entrance);
struct FieldTransition {int scene;unsigned entrance,countdown=2;bool ready=false;};
struct FieldEncounter {unsigned column,mode;bool rear_attack;};
struct FieldCameraSetting {int id=0;float height=0,scale=0,target=0,mode=0;};
std::vector<FieldCameraSetting> field_camera_settings(Bytes stg);
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
                 std::function<bool(int)> wait_dialogue,std::span<const ActorBinding> actors={},bool diagnostic=true);
    void tick(ExplorationInput input={});
    void load_effects(Bytes,std::shared_ptr<const EffectTemplatePreparation> prepared={});
    void load_map_effects(Bytes map,const EffectTextureSizes&);
    void bind_map_objects(std::span<const ActorBinding>);
    void bind_map_scene(MapObjects&);
    void load_exploration_camera(Bytes stg);
    void load_exploration_collision(Bytes xcl,Bytes variables);
    void load_interactions(Bytes database,const std::string& room,Bytes variables);
    void enter(FieldDefinition definition,Bytes stg,Bytes xcl,unsigned entrance,ExplorationInput input={});
    const std::optional<FieldTransition>& transition() const {return transition_;}
    const std::optional<FieldEncounter>& encounter() const {return encounter_;}
    std::function<std::uint32_t()> random_value;
    // Session-owned manager state persists across field construction.
    ExteriorCamera* exterior_camera=nullptr;
    CollisionMask* collision_mask=&local_collision_mask_;
    std::function<void(unsigned)> transition_service;
    const PlayerMovement& player_movement() const {return player_;}
    const std::optional<ShadowTransition>& player_shadow() const {return shadow_;}
    const std::vector<CollisionBody>& collision_bodies() const {return bodies_;}
    const AttachedEffects& effects() const {return effects_;}
    const MapEffects& map_effects() const {return map_effects_;}
    const EffectSubmissions& effect_submissions() const {return effect_submissions_;}
    Float4 position{},target{},rotation{};
    CameraBlend blend;
    ScreenFilter screen_filter;
    std::function<bool(unsigned,std::int32_t)> audio_service;
    std::function<bool(std::int32_t,bool)> music_service;
    std::function<bool(int,unsigned,unsigned)> music_channels;
    std::function<void(bool)> dialogue_input;
    std::function<void(unsigned,int)> scene_fade;
    std::function<bool(int,bool)> map_visibility;
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
    // Session-owned; retained across field transitions and independent of Metal.
    FieldProjection* render_projection=nullptr;
private:
    CollisionMask local_collision_mask_;
    std::vector<std::uint8_t> bytes_;
    Animation script_;
    SineTable sine_=make_sine_table();
    AtanTable angles_=make_atan_table();
    Float4 requested_target_{};
    std::vector<ActorBinding> actors_;
    std::vector<ActorBinding> map_objects_;
    FieldActor* actor(int alias) const;
    int group_=-2;
    unsigned camera_mode_=2;
    FieldActor* camera_actor_=nullptr;
    std::optional<AnchoredCamera> exploration_camera_;
    std::vector<FieldCameraSetting> camera_settings_;
    bool exterior_camera_enabled_=false;
    unsigned configured_camera_area_=0;
    void configure_exterior_camera(unsigned area);
    bool exploration_handoff_=false;
    std::optional<CollisionMap> collision_;
    std::vector<CollisionBody> bodies_;
    std::vector<unsigned> actor_collision_indices_;
    MapObjects* map_scene_=nullptr;
    unsigned map_body_count_=0;
    void sync_collision_positions();
    void commit_collision_positions(unsigned mover);
    PlayerMovement player_;
    std::optional<ShadowTransition> shadow_;
    void update_player_shadow();
    std::optional<FieldDefinition> definition_;
    int interaction_alias_=0,interaction_event_=0;
    std::int32_t saved_input_gate_=0;
    bool input_gate_saved_=false;
    void restore_event_gate();
    std::optional<FieldTransition> transition_;
    std::optional<FieldEncounter> encounter_;
    std::vector<std::optional<EncounterEnemy>> enemies_;
    bool rear_attack_=false;
    void initialize_npcs();
    void update_npcs();
    void clear_contact();
    void build_collision(Bytes xcl);
    void advance_effects();
    void interact(ExplorationInput);
    void move_player(ExplorationInput);
    void remember_player();
    void tick_callbacks(ExplorationInput input,bool interior_camera);
    std::vector<EffectDefinition> effect_definitions_;
    std::shared_ptr<const EffectTemplatePreparation> effect_preparation_;
    void bind_effect_geometry(MapEffect&) const;
    AttachedEffects effects_;
    MapEffects map_effects_;
    EffectSubmissions effect_submissions_;
    bool map_effects_loaded_=false;
    EffectTextureSizes effect_texture_sizes_{};
    void spawn_dust(const Float4& position);
    unsigned spawn_positioned_effect(unsigned template_id,const Float4& position);
    unsigned attach_map_effect(unsigned template_id,int alias);
    std::array<unsigned,16> effect_ring_{};
    unsigned effect_ring_index_=0;
    unsigned attach_bubble(int parameter,int alias);
    std::function<void(int)> start_dialogue_;
    std::function<bool(int)> wait_dialogue_;
    Animation::ServiceResult service(unsigned,unsigned,std::int32_t,std::uint32_t);
    std::uint32_t word(std::size_t) const;
    Float4 point(std::size_t) const;
    void evaluate();
};
// Reviewed STG light0/base/ambient records for ordinary effect tint.
Float4 field_effect_tint(Bytes stg);
Float4 field_projection(Bytes stg,float* far_clip=nullptr);
// STG0x46 overrides the default camera-relative sky translation factors.
Float4 field_sky_follow(Bytes stg);
AnchoredCamera field_anchored_camera(Bytes stg);
}
