#pragma once
#include "okage/battle_actor.hpp"
#include "okage/dialogue.hpp"

namespace okage {
struct BattleVictoryServices {
    std::function<std::uint32_t()> random;
    std::function<void(unsigned)> camera;
    std::function<void()> stop_music;
    std::function<void(unsigned)> jingle;
    std::function<void(unsigned,int)> fade_ui;
};
void battle_victory_update(BattleState&,std::span<const BattleEntityBinding>,
                           SceneFade&,std::span<std::uint8_t> reward,
                           const BattleVictoryServices&);
void battle_defeat_update(BattleState&,SceneFade&,
                          const std::function<void(unsigned,int)>& fade_ui);
}
