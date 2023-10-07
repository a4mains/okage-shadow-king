#pragma once

#include "okage/battle_hud.hpp"

#include <array>
#include <span>
#include <functional>

namespace okage {

struct ResultsProgress {
    std::array<std::uint8_t,0x5ac> scratch{};
    std::array<std::int32_t,3> notice_counters{{-1,-1,-1}};
    std::uint32_t notice=0,notice_row=0,notice_wait=0;

    // 0x169900 source-order snapshot/XP initialization followed by the
    // constructor's initial 0x16aac0 and 0x16b5a0 calls for each party row.
    void initialize(std::span<std::uint8_t> variables,const GameDataTables&,
                    const UiPartySelection&,std::span<std::uint8_t> reward,
                    RuwResource&,const BattleHudData&,unsigned game_mode,
                    const std::function<void(unsigned)>& play_jingle);

    // 0x16aac0 for one source party row. False means more progression remains;
    // true means the source reached its completion return for this row.
    bool update_character(unsigned row,bool initial,
                          std::span<std::uint8_t> variables,const GameDataTables&,
                          const UiPartySelection&,RuwResource&,const BattleHudData&,
                          const std::function<void(unsigned)>& play_jingle);

    // 0x16b5a0: hide the six paired stat rows, preserving all other RUW state.
    void hide_changed(RuwResource&,unsigned row) const;
};

} // namespace okage
