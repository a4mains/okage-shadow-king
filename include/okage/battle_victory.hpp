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
struct BattleEscapeServices {
    std::function<void(unsigned row)> camera;                      //1510a0 table row
    std::function<void(unsigned,int)> fade_ui;                     //11fdf8(root+8060,1,-128)
    std::function<void(std::array<std::uint32_t,3>)> game_command; //1248d8, party only
    std::function<void()> stop_music;                              //12d470, enemy only
};
void battle_escape_update(BattleState&,std::int32_t command,SceneFade&,
                          std::span<std::uint8_t> reward,const BattleEscapeServices&);
}
