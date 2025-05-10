#pragma once
#include "game_over_session.hpp"
#include "okage/title.hpp"
#include "okage/scene_renderer.hpp"

namespace okage {
struct TitleConstruction {
    std::filesystem::path common_root,effect_textures;
    std::shared_ptr<const SceneRenderer::EffectTextureBytes> verified_effect_textures;
    Bytes effect_data;
    std::function<void()> stop_music,start_theme;
};
struct TitleActions {
    std::function<void(GameModeSelection)> request;
    std::function<void(unsigned)> sound;
    std::function<void()> reset_game,reset_progress;
    std::function<void(unsigned)> select_saved_field;
    std::function<std::array<std::uint32_t,2>(unsigned)> read_saved_field;
};
class TitleSession {
public:
    TitleSession(SceneClearColor&,SceneFade&,BattleUiShared&,GameOverShared&,TitleConstruction);
    ~TitleSession();
    void tick(GameOverPadSample,std::uint32_t confirm_mask,GameOverShared&,SceneFade&,Animation&,
        FieldProjection&,RetiringSceneEffects*,BattleUiShared&,
        std::shared_ptr<const std::vector<Font>>,const SineTable&,GameRandom&,const TitleActions&);
    RetiringSceneEffects retire(Animation&,FieldProjection&,BattleUiShared&);
    bool resource_live() const;
    const Texture* rendered_image() const;
    const TitleTaskState& state() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
