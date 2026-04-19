#pragma once
#include "okage/battle_actor.hpp"

namespace okage {
struct BattleHitServices {
    std::function<unsigned(int,BattleActor&)> attached_effect;
    std::function<void(unsigned,int)> effect_value;
    std::function<void(BattleActor&,std::int16_t,std::int16_t)> history;
    std::function<void(BattleActor&)> death_history;
    std::function<unsigned(int,const Float4&,bool source_w_available)> positioned_effect;
    std::function<std::int16_t()> entry_escape_flag;
    std::function<std::uint32_t()> command_available;
    std::function<void(std::int32_t owner,std::int32_t command)> command_push;
    std::function<void(std::int32_t)> spend_money;
};
int battle_apply_hit(BattleActor&,std::int16_t category,std::int16_t amount,
                     const BattleHitServices&);
}
