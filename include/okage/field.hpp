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
#include "okage/image_quad.hpp"
#include <optional>
namespace okage {
class MapObjects;
struct ExplorationInput {std::uint16_t held=0;std::uint8_t x=128,z=128;std::uint16_t pressed=0;std::uint8_t look_x=128;};
struct FieldNpcDefinition {
    int alias=0;std::string model;std::int32_t state=0,talk=0,alternate_talk=0;
    Float4 position{};float facing=0;unsigned flags=1;
};
struct FieldDefinition {
    // party is source manager+0x80: both follower enable and bunpu row.
    // Keep its full authored value; it is not a boolean or roster count.
    int scene=-1;std::uint32_t type=0,party=0,startup=0,field_flags=0;
    std::int32_t music=0,secondary_music=0;
    std::int32_t battle_record_selection=-1;
    unsigned footstep=33,dust=0;
    unsigned navigation_flags=0;
    float navigation_scale=2,navigation_offset_x=0,navigation_offset_z=0; //+418..424 after entry0
    std::string room_directory,room,message,event;
    std::vector<FieldNpcDefinition> npcs;
    std::vector<unsigned> followers;
};
struct FieldConstructorPlan {
    std::uint32_t phase=7;
    bool clears_temporary_flags=true;
    std::int32_t scene=-1,entrance=-1;
    bool saved_pose=false;
};
FieldConstructorPlan plan_field_constructor(const std::array<std::uint32_t,3>& selection,
                                            const std::array<std::uint32_t,3>& command,
                                            std::optional<std::int32_t> saved_scene);
FieldDefinition read_field_definition(Bytes database,const std::string& room);
FieldDefinition read_field_definition(Bytes database,int scene);
// 0x131d28 defaults followed by synchronous entry zero. This is field setup,
// not cold game initialization; inputs must already contain scene/entrance/gate.
std::array<std::uint8_t,0x800> field_setup_state(Bytes script,Bytes variables,const FieldDefinition& definition);
FieldDefinition prepare_field_definition(Bytes script,Bytes variables,FieldDefinition definition);
//131ee0..131f34 before NAVI;13218c+ event load/entry0 follows its resources.
FieldDefinition begin_field_load(std::span<std::uint8_t> variables,FieldDefinition definition,std::int32_t entrance);
FieldDefinition finish_field_load(Bytes script,Bytes variables,FieldDefinition definition);
FieldDefinition prepare_field_load(Bytes script,std::span<std::uint8_t> variables,
                                   FieldDefinition definition,unsigned entrance);
// Convenience entry path retains its original125ad0 clear before preparation.
FieldDefinition prepare_field_entry(Bytes script,std::span<std::uint8_t> variables,
                                    FieldDefinition definition,unsigned entrance);
// Command-13 return preparation: keep temporary flags, select the current
// scene, and use signed entrance -1 so 0x132720 restores the saved pose.
FieldDefinition prepare_field_return(Bytes script,std::span<std::uint8_t> variables,
                                     FieldDefinition definition);
struct FieldEntrance {Float4 position{},rotation{};};
FieldEntrance field_entrance(Bytes stg,unsigned entrance);
struct FieldTransition {int scene;unsigned entrance,countdown=2;bool ready=false;};
struct FieldEncounter {
    unsigned column,mode;bool rear_attack;
    unsigned row;std::int32_t formation;
};
struct FieldBattleRequest {
    unsigned formation;std::int32_t stage;unsigned return_token;
    unsigned phase=0;bool dispatched=false;
    std::uint32_t entry_parameter=0;bool rear_attack=false;
};
// 0x1301a8 return pose plus the entered field's phase13 music selection.
// The camera value is channel0 before CameraBlend, not the rendered position.
struct FieldBattleReturnSnapshot {
    Float4 player_position{},player_rotation{},raw_camera_position{};
    int scene=-1;
    std::int32_t secondary_music=0;
    std::int32_t battle_record_selection=-1;
};
struct FieldEventActor {FieldActor* actor=nullptr;std::vector<std::uint8_t> camera_motion;};
enum class FieldEncounterChildStep {
    CheckLiveChild,
    Construct,        // after 0x1301a8: 125918(13,4,0), RNG, 11f570, 100ac0(res,0)
    Finished,         // phase9: child null or +1c negative
    Restore,          // phase9 completion: 100ac0(res,1)
    ConstructRear,
                      // 11f5f8(manager,0x2226f0) -> +50, 115b50(0x2284a0)
    RetireRear        // phase10 with timer 0: +50 child +1c |= 0x80000000
};
struct FieldAudioRequest {
    unsigned operation=0;
    std::int32_t argument=0;
    std::optional<std::int32_t> primary_music;
};
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
struct OpeningFieldTestAccess;
class OpeningField {
public:
    // Opt-in diagnostic over the field event script (Animation::set_trace_callback).
    void set_event_trace(std::function<void(std::uint32_t,unsigned)> trace){script_.set_trace_callback(std::move(trace));}
    OpeningField(Bytes script,std::function<void(int)> start_dialogue,
                 std::function<bool(int)> wait_dialogue,std::span<const ActorBinding> actors={},bool diagnostic=true,
                 bool defer_actor_setup=false);
    void setup_actor(std::size_t index);
    void tick(ExplorationInput input={});
    void load_effects(Bytes,std::shared_ptr<const EffectTemplatePreparation> prepared={});
    void load_map_effects(Bytes map,const EffectTextureSizes&);
    void bind_map_objects(std::span<const ActorBinding>);
    void bind_map_scene(MapObjects&);
    void load_exploration_camera(Bytes stg);
    void load_exploration_collision(Bytes xcl,Bytes variables);
    void load_interactions(Bytes database,const std::string& room,Bytes variables);
    void enter(FieldDefinition definition,Bytes stg,Bytes xcl,unsigned entrance,ExplorationInput input={});
    // Split 0x131d28 construction from 0x132720 activation. Staging owns copies
    // of the STG/XCL bytes so the activation can happen in a later manager turn.
    void prepare(FieldDefinition definition,Bytes stg,Bytes xcl);
    void begin_prepare(FieldDefinition definition,Bytes stg,Bytes xcl);
    void prepare_next_npc();
    void finish_prepare();
    void activate(unsigned entrance);
    void activate_entry(unsigned entrance,ExplorationInput input={},const std::function<void()>& after_activate={});
    void activate_return(const FieldBattleReturnSnapshot& saved,ExplorationInput input={},
                         const std::function<void()>& after_activate={});
    const std::optional<FieldTransition>& transition() const {return transition_;}
    const std::optional<FieldEncounter>& encounter() const {return encounter_;}
    const std::optional<FieldBattleRequest>& battle_request() const {return battle_request_;}
    // Call during the phase13 manager callback, before later script/channel
    // updates. Like0x1b43c0, reads evaluate dirty channels without advancing them.
    FieldBattleReturnSnapshot battle_return_snapshot();
    // Phase13 samples these live values only for the source stage -1 sentinel.
    std::int32_t default_battle_stage() const;
    void validate_retirement() const;
    RetiringSceneEffects retire_effects();
    // Manager +48 is 0 (case0 exploration): activation, or 132ca8 after case5.
    bool exploration_ready() const {return manager_phase_==0;}
    std::uint32_t manager_event_phase() const {return manager_phase_;}
    std::function<void()> dispatch_game_command;
    //133b3c only while command dispatch leaves phase0, before follow/actors.
    std::function<void(ExplorationInput)> phase0_image_commands;
    std::function<void()> open_save_menu;
    std::function<void()> recover_party;
    std::function<void(std::string_view)> set_save_caption;
    bool save_requested() const {return save_requested_;}
    bool save_menu_active() const {return save_menu_active_;}
    void close_save_menu();
    std::int32_t persistent_map_index() const {return definition_?definition_->battle_record_selection:-1;}
    std::function<void(unsigned,FieldBattleRequest&)> battle_transition_service;
    // Manager descendants (the +50 child): traversed right after the 0x133a98
    // callback of an ordinary manager tick, before the global script pass.
    std::function<void()> manager_children;
    std::function<void()> resource_ui_pass;
    std::function<bool(FieldEncounterChildStep,FieldBattleRequest&)> encounter_child_service;
    // Phase9 0x132f00 queues70 image commands for the +104 mapinfo2 second cell.
    // Valid only during that manager callback's traversal; Session drains them.
    const std::vector<ImageQuad>& encounter_overlay() const {return encounter_overlay_;}
    bool encounter_overlay_active() const {return !encounter_overlay_.empty();}
    // Manager +108: phase11 rear contact sets 90; phase10 counts it down.
    std::int32_t encounter_rear_timer() const {return encounter_rear_timer_;}
    std::function<std::int32_t(unsigned,unsigned)> resolve_encounter_formation;
    std::function<std::uint32_t()> random_value;
    // Optional read-only research observer, before/after a state-1 callback.
    std::function<void(int,bool,const WanderingNpc&,const Animation&)> observe_wander;
    // Session-owned manager state persists across field construction.
    ExteriorCamera* exterior_camera=nullptr;
    CollisionMask* collision_mask=&local_collision_mask_;
    std::function<void(unsigned)> transition_service;
    std::function<void(unsigned,std::uint32_t)> request_chapter;
    std::int32_t chapter_selector() const {return chapter_selector_;}
    const PlayerMovement& player_movement() const {return player_;}
    const Animation& camera_channels() const {return script_;}
    const std::optional<ShadowTransition>& player_shadow() const {return shadow_;}
    // 0x1300d0 -> 0x1346e0: dialogue SHADOW requests use the transition
    // switch directly, without event54's bit0x100 selector override.
    bool request_player_shadow(std::uint32_t request);
    bool request_actor_motion(int alias,unsigned entry);
    const std::vector<CollisionBody>& collision_bodies() const {return bodies_;}
    const AttachedEffects& effects() const {return effects_;}
    const MapEffects& map_effects() const {return map_effects_;}
    const EffectSubmissions& effect_submissions() const {return effect_submissions_;}
    Float4 position{},target{},rotation{};
    CameraBlend blend;
    ScreenFilter screen_filter;
    std::function<bool(const FieldAudioRequest&)> audio_service;
    std::function<bool(std::int32_t,bool)> music_service;
    std::function<bool(int,unsigned,unsigned)> music_channels;
    std::function<void(bool)> dialogue_input;
    // Owned event resources; Session retains the returned actor through field teardown.
    std::function<FieldEventActor()> create_hat_scene;
    std::function<std::optional<int>(int)> resolve_instance_alias;
    std::function<void(int,FieldActor*)> set_instance_alias;
    std::function<void(unsigned,int)> scene_fade;
    std::function<void()> activation_ready;
    std::function<bool(int,bool)> map_visibility;
    std::function<bool(unsigned,std::int32_t,float,unsigned)> sound_cue;
    std::function<bool()> stop_sound_cues;
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
    friend struct OpeningFieldTestAccess;
    CollisionMask local_collision_mask_;
    std::vector<std::uint8_t> bytes_;
    Animation script_;
    std::shared_ptr<AnimationChannelPool> channel_pool_;
    SineTable sine_=make_sine_table();
    AtanTable angles_=make_atan_table();
    Float4 requested_target_{};
    std::vector<ActorBinding> actors_;
    std::size_t actors_setup_=0;
    bool defer_actor_setup_=false;
    void setup_npc_pose(const FieldNpcDefinition& npc);
    std::vector<ActorBinding> map_objects_;
    FieldActor* actor(int alias) const;
    int group_=-2;
    unsigned camera_mode_=2;
    FieldActor* camera_actor_=nullptr;
    bool hat_requested_=false;
    FieldActor* hat_actor_=nullptr;
    FieldActor* alias7_actor_=nullptr;
    std::unique_ptr<Animation> camera_motion_;
    std::optional<AnchoredCamera> exploration_camera_;
    std::vector<FieldCameraSetting> camera_settings_;
    bool exterior_camera_enabled_=false;
    unsigned configured_camera_area_=0;
    void configure_exterior_camera(unsigned area);
    std::uint32_t manager_phase_=5;
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
    std::vector<std::uint8_t> staged_stg_,staged_xcl_;
    bool preparing_=false,prepared_=false,activated_=false;
    int interaction_alias_=0,interaction_event_=0;
    std::int32_t saved_input_gate_=0;
    bool save_requested_=false,save_menu_active_=false,save_resuming_=false;
    bool rest_requested_=false;
    bool input_gate_saved_=false;
    void restore_event_gate();
    std::optional<FieldTransition> transition_;
    std::int32_t chapter_selector_=-1;
    std::optional<FieldEncounter> encounter_;
    bool upper_material_known_=false;
    std::optional<FieldBattleRequest> battle_request_;
    // Task member+28, distinct from the parent's scene argument. Ordinary
    // field entry starts at zero; pending mode0xe entry is not implemented.
    std::uint32_t battle_return_value_=0;
    std::vector<std::optional<EncounterEnemy>> enemies_;
    std::vector<std::optional<WanderingNpc>> wanderers_;
    bool rear_attack_=false;
    // 0x133a98 phase8/9: manager+110 overlay angle and the 0x1301a8 raw camera
    // restored by 0x130248 when the child completes.
    float encounter_angle_=0;
    std::int32_t encounter_rear_timer_=0;
    Float4 encounter_saved_camera_{};
    std::vector<ImageQuad> encounter_overlay_;
    void encounter_phase8();
    void encounter_phase9();
    void encounter_phase10();
    void encounter_rear_child();
    void hold_player_for_encounter();
    void initialize_npcs();
    void initialize_one_npc(std::size_t index);
    void activate_pose(const Float4& player_position,const Float4& player_rotation,
                       const std::optional<Float4>& raw_camera_position);
    void update_npcs();
    void clear_contact();
    void build_collision(Bytes xcl);
    void advance_effects();
    void interact(ExplorationInput);
    void move_player(ExplorationInput);
    void remember_player();
    void tick_callbacks(ExplorationInput input,bool interior_camera,bool pause_actors=false,bool frozen_resource=false);
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
// 0x1b8028 defaults and ordered STG1..3/11..13/40/41/45 setters.
struct FieldOrdinaryLighting {
    Float4 base{1,1,1,1};
    std::array<Float4,3> directions{Float4{0,0,1,0},Float4{0,1,0,0},Float4{1,0,0,0}};
    std::array<Float4,4> colors{Float4{1,1,1,0},Float4{},Float4{},Float4{.25f,.25f,.25f,1}};
};
FieldOrdinaryLighting field_ordinary_lighting(Bytes stg);
struct SceneClearColor {
    std::array<std::uint8_t,3> rgb{64,64,64}; //0x1002f8 ->0x1cb4b8.
    // 337b74 written by 1bb088 for an authored STG setting 0x60; 100ac0(res,1)
    // restores the draw environment from it. Absent until such a setting loads.
    std::optional<std::array<std::uint8_t,3>> stage_rgb;
    void load_stage(Bytes);
};
// Reviewed STG light0/base/ambient records for ordinary effect tint.
Float4 field_effect_tint(Bytes stg);
Float4 field_projection(Bytes stg,float* far_clip=nullptr);
// STG0x46 overrides the default camera-relative sky translation factors.
Float4 field_sky_follow(Bytes stg);
AnchoredCamera field_anchored_camera(Bytes stg);
}
