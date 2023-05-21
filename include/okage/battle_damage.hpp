#pragma once
#include "okage/battle_actor.hpp"

namespace okage {
void battle_queue_damage_event(BattleState&,std::span<const BattleEntityBinding>,
    std::int16_t command,std::int16_t owner,std::int16_t target,
    const std::function<std::uint32_t()>& random);
}
