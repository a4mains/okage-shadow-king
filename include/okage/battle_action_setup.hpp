#pragma once
#include "okage/battle_actor.hpp"

namespace okage {
void battle_decode_command(BattleActor&,const BattleState&,
    std::span<const BattleEntityBinding>,const std::function<std::uint32_t()>& random);
//157ed0: ordered auxiliary descriptor setup, also used by ready special actions.
void battle_setup_secondary_action(BattleActor&,const BattleActionData&,std::int16_t action,std::int16_t mode);
void battle_register_target_group(BattleActor&,BattleState&,
    std::span<const BattleEntityBinding> bindings,std::uint32_t flag);
float battle_setup_action(BattleActor&,BattleState&,const GameDataTables&,const BattleActionData&,
                          std::span<const BattleEntityBinding> bindings={});
}
