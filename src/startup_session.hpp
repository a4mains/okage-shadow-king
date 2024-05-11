#pragma once
#include "game_over_session.hpp"
#include "okage/startup.hpp"
#include <filesystem>
namespace okage {
struct StartupConstruction {
    unsigned selector;
    std::filesystem::path save_directory;
    std::function<void()> stop_music,initialize_shared;
    std::function<std::vector<std::uint8_t>(unsigned)> load_image;
};
// Native storage service and logo resources for source mode8 selectors3/0.
class StartupSession {
public:
    StartupSession(SceneClearColor&,SceneFade&,BattleUiShared&,GameOverShared&,StartupConstruction);
    ~StartupSession();
    void tick(GameOverPadSample,GameOverShared&,SceneFade&,Animation&,FieldProjection&,
        RetiringSceneEffects*,BattleUiShared&,std::shared_ptr<const std::vector<Font>>,
        const SineTable&,GameRandom&,const std::function<void(GameModeSelection)>&);
    void retire(Animation&,FieldProjection&,RetiringSceneEffects*,BattleUiShared&);
    bool resource_live() const;
    const Texture* rendered_image() const;
    const StartupTaskState& state() const;
    std::array<std::uint32_t,4> storage_observation() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
