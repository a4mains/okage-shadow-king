#pragma once
#include <cstdint>
#include <functional>
#include <span>

namespace okage {
void initialize_battle_actor_timer(std::span<std::uint8_t> storage,
    int parameter, unsigned kind, int entry_half32,
    const std::function<std::uint32_t()>& random);

void initialize_battle_enemy_selector(std::span<std::uint8_t> body_from_10c,
    std::int32_t current_value, int formation_count,
    const std::function<std::uint32_t()>& random);
}
