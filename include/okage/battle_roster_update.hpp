#pragma once
#include "okage/battle_actor.hpp"
#include <functional>

namespace okage {
class BattleEnemyCommands;
enum class BattleRosterSound {Ready,Cycle};
struct BattleRosterUpdateContext {
    BattleState& root;
    BattleEntryContext& entry;
    Bytes variables;
    std::span<const BattleEntityBinding> entities;
    std::uint16_t pad_pressed=0;
    std::uint32_t selection_buttons=0;
    std::function<void(BattleRosterSound)> sound;
    //157ce8 consumes once for a nonempty target group. Status3/5 still guard.
    std::function<std::uint32_t()> random;
    const GameDataTables* tables=nullptr;
    const BattleActionData* action_data=nullptr;
    const BattleEnemyCommands* enemy_commands=nullptr;
};
void battle_party_roster_update(std::span<BattleActor* const> ordinary,
                                BattleActor& stan,BattleRosterUpdateContext&);
//15afa0 phase1:15b0a8 for each non-null authored enemy slot in order.
void battle_enemy_roster_update(std::span<BattleActor* const> enemies,
                                BattleRosterUpdateContext&);
}
