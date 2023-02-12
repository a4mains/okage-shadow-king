#pragma once
#include "okage/battle_actor.hpp"
#include "okage/battle_state.hpp"
#include <cstdint>
#include <span>

namespace okage {
// 0x154770: signed status binding1 and signed counter binding2. A null
// actor follows the source helper's null=>incapacitated result.
bool battle_actor_incapacitated(const BattleActor* actor);

// 0x1517e0/0x151978/0x151858. The queue is stored in BattleState's recovered
// secondary table. Push preserves the source's saturating top-index behavior.
void battle_command_push(BattleState&,std::int32_t owner_index,std::int32_t command);
std::uint32_t battle_command_available(const BattleState&);
std::int32_t battle_command_pop(BattleState&,std::span<const BattleEntityBinding>,
                                std::int32_t& owner_out);
}
