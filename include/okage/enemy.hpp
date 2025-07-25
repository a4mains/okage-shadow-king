#pragma once
#include "okage/camera.hpp"
#include <functional>
#include <optional>

namespace okage {
// Retail 0x1f14b0. One owner must supply this stream to all game systems.
// Initial seed and other callers belong to game initialization, not NPCs.
struct GameRandom {
    std::uint32_t state;
    // Optional observation of every draw (state after the draw); never changes values.
    void (*observe)(void* context,std::uint32_t state)=nullptr;
    void* observe_context=nullptr;
    std::uint32_t next(){state=state*0x41c64e6du+0x3039u;if(observe)observe(observe_context,state);return state&0x7fffffffu;}
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
// Retail ordinary NPC state 1 (0x137df8). Collision belongs to the field;
// initialization selects a leg without moving, movement then reports its
// corrected position back through finish_move().
struct WanderingNpc {
    std::int32_t counter=0;
    Float4 velocity{};
    Float4 begin_leg(const Float4& position,float target_w,
                    const std::function<std::uint32_t()>& random,const AtanTable&);
    void finish_move(const Float4& before,const Float4& after);
};
}
