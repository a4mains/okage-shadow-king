#pragma once
#include "okage/battle_state.hpp"
#include "okage/game_mode.hpp"
#include <exception>

namespace okage {
class BattleRootLifetime {
public:
    void wrapper_update(const BattleState&,const std::function<void(GameModeSelection)>& request) const;
    void traverse_root(BattleState&,const std::function<void()>& normal,
                       const std::function<void()>& destroy,
                       const std::function<void()>& release);
    bool linked() const {return linked_;}
private:
    bool linked_=true;
    std::exception_ptr failure_;
};
}
