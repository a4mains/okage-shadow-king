#pragma once
#include "okage/battle_actor.hpp"

namespace okage {
void battle_enemy_reward(BattleActor&,std::span<const BattleEntityBinding>,
                         std::span<std::uint8_t> reward_record,
                         const std::function<std::uint32_t()>& random);
}
