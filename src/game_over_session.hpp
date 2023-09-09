#pragma once
#include "okage/game_over.hpp"
#include "okage/scene_resource.hpp"
#include <memory>

namespace okage {
//Mode8 input scratch is BSS at startup and survives resource/task retirement.
struct GameOverShared {
    GameOverInputState input{0,{0x1000,0x2000,0x4000,0x8000},{},{}};
    std::array<std::uint32_t,6> mode_words{}; //259908..25991c
    std::uint32_t menu_word=0; //1fb290
    std::uint32_t image_frame_counter=0;
    void initialize_mode() {mode_words={0,9999,0,0,0,0};menu_word=0;}
};
struct GameOverConstruction {
    std::function<void()> stop_music;
    std::function<std::vector<std::uint8_t>()> load_image;
};
class GameOverSession {
public:
    GameOverSession(SceneClearColor&,SceneFade&,BattleUiShared&,GameOverShared&,GameOverConstruction);
    ~GameOverSession();
    GameOverSession(const GameOverSession&)=delete;
    GameOverSession& operator=(const GameOverSession&)=delete;
    void tick(GameOverPadSample,GameOverShared&,SceneFade&,Animation&,FieldProjection&,
        RetiringSceneEffects*,BattleUiShared&,std::shared_ptr<const std::vector<Font>>,
        const SineTable&,GameRandom&,const std::function<void(GameModeSelection)>&);
    void retire(Animation&,FieldProjection&,RetiringSceneEffects*,BattleUiShared&);
    bool resource_live() const;
    const Texture* rendered_image() const;
    const GameOverTaskState& state() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
