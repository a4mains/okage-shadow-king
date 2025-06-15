#pragma once
#include "game_over_session.hpp"
#include "okage/title.hpp"
#include "okage/scene_renderer.hpp"

namespace okage {
// 12c350(1) image order (0x12c430..0x12c4a0).
inline constexpr std::array<const char*,8> title_image_names{"title_sample4.xim","push.xim","new.xim","restore.xim",
    "maru_w.xim","maru_b.xim","sp.xim","hanten.xim"};
struct TitleConstruction {
    std::filesystem::path common_root,effect_textures;
    std::shared_ptr<const SceneRenderer::EffectTextureBytes> verified_effect_textures;
    // Cold startup: the SCR bytes the Session already loaded logically (title_image_names order).
    std::shared_ptr<const std::array<std::vector<std::uint8_t>,8>> images;
    // Cold startup: the NIGHT_V a.xmd bytes the Session loaded logically.
    std::shared_ptr<const std::vector<std::uint8_t>> model;
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> model_textures;
    Bytes effect_data;
    std::function<void()> stop_music,start_theme;
};
struct TitleEffectEvent {
    enum class Op : std::uint8_t {Create,Bind,RemovalRequest} op;
    unsigned slot{},definition{},element{},image_index{};
};
struct TitleActions {
    std::function<void(GameModeSelection)> request;
    std::function<void(unsigned)> sound;
    std::function<void()> reset_game,reset_progress;
    std::function<void(unsigned)> select_saved_field;
    std::function<std::array<std::uint32_t,2>(unsigned)> read_saved_field;
    std::function<void(const TitleEffectEvent&)> observe_effect{}; // optional, observational only
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
