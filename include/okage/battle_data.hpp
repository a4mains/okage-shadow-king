#pragma once
#include "okage/assets.hpp"
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
}
