#pragma once
#include "okage/battle_actor.hpp"

namespace okage {
void battle_decode_command(BattleActor&,const BattleState&,
    std::span<const BattleEntityBinding>,const std::function<std::uint32_t()>& random);
float battle_setup_action(BattleActor&,BattleState&,const GameDataTables&,const BattleActionData&);
}
