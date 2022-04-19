#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace okage {
inline constexpr std::size_t battle_state_size=0x81d0;
inline constexpr std::size_t battle_entity_count=64;
inline constexpr std::size_t battle_entity_size=0x200;

void initialize_battle_entities(std::span<std::uint8_t> root,
                                std::span<const std::uint8_t> default_bytes);
void initialize_battle_secondary(std::span<std::uint8_t> root);

// Scalar arguments copied by 0x149fd8 from its wrapper. The wrapper owns
// stage clamping; this helper preserves the supplied bits without clamping.
struct BattleRootParameters {
    float scale;
    std::uint32_t formation;
    std::int32_t stage;
};
void initialize_battle_root(std::span<std::uint8_t> root,const BattleRootParameters&);

// Native ownership only; no guest addresses, callbacks or battle scheduler.
struct BattleState {
    std::array<std::uint8_t,battle_state_size> bytes{};
    void initialize_root(const BattleRootParameters& parameters) {
        initialize_battle_root(bytes,parameters);
    }
    void initialize_entities(std::span<const std::uint8_t> defaults) {
        initialize_battle_entities(bytes,defaults);
    }
    void initialize_secondary() {initialize_battle_secondary(bytes);}
    std::span<std::uint8_t,battle_entity_size> entity(std::size_t index);
    std::span<const std::uint8_t,battle_entity_size> entity(std::size_t index) const;
};
}
