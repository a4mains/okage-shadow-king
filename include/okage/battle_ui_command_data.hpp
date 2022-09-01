#pragma once
#include "okage/assets.hpp"
#include <array>
#include <cstdint>

namespace okage {
class BattleUiCommandData {
public:
    explicit BattleUiCommandData(Bytes);
    const std::array<std::int32_t,2>& category_zero_commands() const { return category_zero_; }
    const std::array<std::int32_t,2>& category_nonzero_commands() const { return category_nonzero_; }
    const std::array<std::int32_t,4>& selection_commands() const { return selection_; }
    const std::array<std::int32_t,4>& authored_metrics_default() const { return metrics_; }
    const std::array<std::int32_t,4>& authored_clip_default() const { return clip_; }
private:
    std::array<std::int32_t,2> category_zero_{};
    std::array<std::int32_t,2> category_nonzero_{};
    std::array<std::int32_t,4> selection_{};
    std::array<std::int32_t,4> metrics_{};
    std::array<std::int32_t,4> clip_{};
};
}
