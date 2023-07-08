#pragma once
#include "okage/battle_actor.hpp"

namespace okage {
struct BattleHitServices {
    std::function<unsigned(int,BattleActor&)> attached_effect;
    std::function<void(unsigned,int)> effect_value;
    std::function<void(BattleActor&,std::int16_t,std::int16_t)> history;
    std::function<void(BattleActor&)> death_history;
    std::function<unsigned(int,const Float4&,bool source_w_available)> positioned_effect;
};
int battle_apply_hit(BattleActor&,std::int16_t category,std::int16_t amount,
                     const BattleHitServices&);
}
