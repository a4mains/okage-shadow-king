#pragma once
#include "okage/battle_hud.hpp"
#include "okage/battle_ui_command_data.hpp"
#include "okage/battle_ui_roster.hpp"

namespace okage {
void update_battle_target_list(RuwResource&,BattleHud&,const BattleHudData&,
    const BattleUiCommandData&,const GameDataTables&,const BattleUiFormationSummary&);
void override_battle_target_display(RuwResource&,const BattleUiCommandData&,
    const GameDataTables&,std::int32_t category,std::int32_t option,
    std::int32_t available,bool selected_action);
}
