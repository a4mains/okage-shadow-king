#pragma once
#include "okage/battle_actor.hpp"
#include "okage/battle_enemy_commands.hpp"

namespace okage {
void battle_decode_enemy_command(BattleActor&,BattleState&,std::span<const BattleEntityBinding>,
    const GameDataTables&,const BattleEnemyCommands&,const std::function<std::uint32_t()>& random);
}
