#pragma once
#include "okage/camera.hpp"
#include <functional>
#include <optional>

namespace okage {
// Retail 0x1f14b0. One owner must supply this stream to all game systems.
// Initial seed and other callers belong to game initialization, not NPCs.
struct GameRandom {
    std::uint32_t state;
    std::uint32_t next(){state=state*0x41c64e6du+0x3039u;return state&0x7fffffffu;}
};
struct EnemyObservation {
    Float4 position{},rotation{},color{},player_position{},player_rotation{},camera_rotation{};
    std::int32_t encounter_gate=1,player_motion=1,field_mode=0,steps=0;
};
struct EnemyActions {
    std::optional<bool> visible;
    std::optional<Float4> position,rotation,color,displacement;
    std::optional<float> alpha;
    bool translucent=false,rear_attack=false;
    std::optional<unsigned> encounter_column;
    unsigned field_mode=0;
};
struct EncounterEnemy {
    std::int32_t counter=0,cooldown=0;
    float speed=-.2f;
    static std::int32_t initial_cooldown(std::uint32_t random,unsigned construction_flags);
    EnemyActions tick(const EnemyObservation&,const std::function<std::uint32_t()>& random,
                      const AtanTable&,const SineTable&);
};
}
