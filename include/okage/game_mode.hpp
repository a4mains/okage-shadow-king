#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace okage {
using GameModeSelection=std::array<std::uint32_t,3>;
class GameModeParent {
public:
    explicit GameModeParent(GameModeSelection initial):current_(initial) {}
    void request(GameModeSelection);
    void tick(const std::function<void()>& destroy_child,
              const std::function<void()>& release_child,
              const std::function<void(GameModeSelection)>& construct_child);
    const GameModeSelection& current() const {return current_;}
    const std::optional<GameModeSelection>& requested() const {return requested_;}
    unsigned phase() const {return phase_;}
    bool has_child() const {return child_;}
    bool has_dead_allocation() const {return dead_;}
private:
    GameModeSelection current_;
    std::optional<GameModeSelection> requested_;
    unsigned phase_=0;
    bool child_=true,dead_=false;
};
}
