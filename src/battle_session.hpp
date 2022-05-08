#pragma once
#include "okage/battle_entry.hpp"
#include "okage/battle_state.hpp"
#include "okage/effect.hpp"
#include "okage/animation.hpp"
#include "okage/game_state.hpp"
#include "okage/enemy.hpp"
#include <filesystem>
#include <memory>

namespace okage {
struct BattleConstructionContext {
    std::span<std::uint8_t> variables;
    const GameDataTables& tables;
    BattleEntryContext& entry;
    GameRandom& random;
};
class BattleSession {
public:
    BattleSession(unsigned formation,int stage,const std::filesystem::path& input,
                  const std::filesystem::path& decoded,Bytes shared_variables,
                  BattleEntryContext& entry,Animation& scene_camera);
    ~BattleSession();
    BattleSession(const BattleSession&)=delete;
    BattleSession& operator=(const BattleSession&)=delete;
    void tick(RetiringSceneEffects*);
    void service_loader_after_frame(BattleConstructionContext,unsigned frame,
                                    const std::filesystem::path& diagnostic_report={});
    const BattleState& state() const;
    std::array<std::uint32_t,7> observation() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
