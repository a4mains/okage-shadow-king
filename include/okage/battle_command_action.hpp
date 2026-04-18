#pragma once
#include "okage/battle_actor.hpp"
#include <functional>

namespace okage {
struct BattleCommandActionServices {
    std::function<void(unsigned row)> camera_index;                  //1510a0(row): anchor -1
    std::function<void(unsigned row,std::int32_t anchor)> camera_anchored; //151100(row,root+81ac)
    std::function<void(int offset)> camera_offset;                   //151170(offset)
    std::function<void()> reset_type3;                                //168458
    std::function<void(bool enemy)> clear_roster_mask;                //115a70
    //157448/157518/1573b8 sound, voice, effect and rotation services.
    BattleStrikeServices strike;
};
enum class BattleCommandActionResult {
    Running,  //151e68 stays installed.
    Restored,
};
BattleCommandActionResult battle_command_action_update(BattleState&,std::span<const BattleEntityBinding>,
                                                       const BattleCommandActionServices&);
}
