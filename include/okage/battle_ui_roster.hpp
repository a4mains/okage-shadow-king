#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace okage {
struct BattleActor;
struct BattleEntityBinding;
struct BattleFormationEntry;
struct BattleState;
class BattleData;
unsigned battle_ui_available_party(std::span<const BattleEntityBinding> entities);
unsigned battle_ui_enemy_group_count(const BattleState& root,
    std::span<const BattleActor* const> enemies,std::int32_t group);

struct BattleUiFormationSummary {
    std::uint32_t count=0; //1f9c94
    std::array<std::int32_t,8> unit_ids{},counts{},groups{}; //1facb8/d8/f8
    void refresh(const BattleState&,const BattleData&,
                 std::span<const BattleActor* const> enemies,unsigned mode);
    void refresh_entries(std::span<const BattleFormationEntry>,const BattleState&,
                         std::span<const BattleActor* const> enemies,unsigned mode);
};
}
