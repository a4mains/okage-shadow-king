#pragma once
#include "okage/assets.hpp"
#include "okage/battle_camera.hpp"
#include <array>
#include <span>
#include <string>
#include <vector>

namespace okage {
struct BattleFormationEntry {
    std::int16_t unit_id;
    std::int8_t field_2, field_3;
    float x, z;
};
class BattleData {
public:
    explicit BattleData(Bytes bytes);
    bool has_formation(int index) const;
    // Rejects out-of-range and absent (-1 count) slots; preserves entry order.
    std::span<const BattleFormationEntry> formation(int index) const;
    const std::string& stage_path(int index) const;
private:
    struct Range { std::uint32_t first, count; };
    std::array<Range,256> formations_{};
    std::array<std::string,23> stages_{};
    std::vector<BattleFormationEntry> entries_;
};
// Pointer-free camera rows imported from owned ELF table0x235ea0. Decode only
// the selected sequence: unreached camera modes remain guarded by its reader.
class BattleCameraData {
public:
    explicit BattleCameraData(Bytes);
    BattleCameraSequence sequence(int stage,bool after_intro=false) const;
    BattleCameraSequence sequence_at_offset(int stage,unsigned row_offset) const;
    BattleCameraSequence sequence_by_index(unsigned table_index) const;
private:
    using Preset=std::optional<std::array<std::uint8_t,0x90>>;
    std::array<std::array<std::array<Preset,3>,5>,23> stages_;
    std::array<std::array<Preset,3>,21> indexed_rows_;
    unsigned rows_per_stage_=0;
    unsigned indexed_row_count_=0;
};
}
