#pragma once
#include "okage/battle_entry.hpp"
#include "okage/battle_state.hpp"
#include "okage/effect.hpp"
#include "okage/animation.hpp"
#include "okage/game_state.hpp"
#include "okage/enemy.hpp"
#include "okage/battle_interface.hpp"
#include "okage/dialogue.hpp"
#include "okage/battle_roster_update.hpp"
#include "okage/game_mode.hpp"
#include <filesystem>
#include <memory>
#include <iosfwd>

namespace okage {
class FieldProjection;
struct SceneClearColor;
struct BattleConstructionContext {
    std::span<std::uint8_t> variables;
    const GameDataTables& tables;
    BattleEntryContext& entry;
    GameRandom& random;
};
struct BattleUiContext {
    std::span<std::uint8_t> variables;
    const GameDataTables& tables;
    UiPartySelection& party;
    BattleUiShared& shared;
    BattleUiPad& pad;
    BattleUiPadInput input;
    SceneFade& fade;
    std::shared_ptr<const std::vector<Font>> fonts;
    const SineTable& sine;
    GameRandom& random;
    //114df8 mutates the persistent outer capture owner, if it exists.
    std::function<void()> release_capture;
    BattleEntryContext& entry;
    //12da90 uses the persistent12da78 owner in the outer Session.
    std::function<void(bool)> music_variant;
    std::function<void(BattleRosterSound)> sound;
    std::function<void(unsigned)> menu_sound;
    std::ostream* live_trace=nullptr;
    std::function<void(int)> strike_sound;
    std::function<void()> stop_music;
    std::function<void(unsigned)> jingle;
    //1248d8 publishes the persistent game command, distinct from mode selection.
    std::function<void(std::array<std::uint32_t,3>)> game_command;
    std::function<void(GameModeSelection)> request_mode;
};
class BattleSession {
public:
    BattleSession(unsigned formation,int stage,const std::filesystem::path& input,
                  const std::filesystem::path& decoded,Bytes shared_variables,
                  BattleEntryContext& entry,Animation& scene_camera,Bytes effect_data,
                  const std::filesystem::path& effect_textures,
                  std::shared_ptr<const EffectTemplatePreparation> effect_preparation,
                  std::size_t observation_capacity=0);
    ~BattleSession();
    BattleSession(const BattleSession&)=delete;
    BattleSession& operator=(const BattleSession&)=delete;
    void tick(RetiringSceneEffects*,FieldProjection&,SceneClearColor&,BattleUiContext,unsigned frame,
              const std::filesystem::path& stage_report={},const std::filesystem::path& ui_update_report={},
              bool defer_readback=false);
    void service_loader_after_frame(BattleConstructionContext,unsigned frame,
                                    const std::filesystem::path& diagnostic_report={},
                                    bool external_loading_windows=false);
    void pause_loader();
    void resume_loader();
    const BattleState& state() const;
    RetiringSceneEffects retire_resource(BattleUiShared&,FieldProjection&);
    bool resource_live() const;
    //Read-only endpoint observation; float lanes are raw bits, no simulation.
    void write_effect_observation(std::ostream&,unsigned frame) const;
    void write_actor_observation(std::ostream&,unsigned frame) const;
    void write_load_observation(std::ostream&) const;
    bool render_pending() const;
    void complete_render(unsigned frame);
    const Texture* rendered_image() const;
    std::array<std::uint32_t,7> observation() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
