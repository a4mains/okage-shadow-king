#pragma once
#include "okage/assets.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace okage {
class BattleUiCommandData {
public:
    explicit BattleUiCommandData(Bytes);
    const std::array<std::int32_t,2>& category_zero_commands() const { return category_zero_; }
    const std::array<std::int32_t,2>& category_nonzero_commands() const { return category_nonzero_; }
    const std::array<std::int32_t,4>& selection_commands() const { return selection_; }
    const std::array<std::int32_t,4>& authored_metrics_default() const { return metrics_; }
    const std::array<std::int32_t,4>& authored_clip_default() const { return clip_; }
    // v2 owned constants consumed by162438/15e900; v1 access rejects.
    const std::array<std::array<std::int32_t,3>,4>& target_palette() const;
    const std::string& target_blank() const;
    std::string target_count_text(std::string_view formatted_number) const;
    std::string target_name_text(std::string_view unit_name) const;
private:
    struct Targets {
        std::array<std::array<std::int32_t,3>,4> palette;
        std::array<std::string,3> formats;
    };
    std::optional<Targets> targets_;
    std::array<std::int32_t,2> category_zero_{};
    std::array<std::int32_t,2> category_nonzero_{};
    std::array<std::int32_t,4> selection_{};
    std::array<std::int32_t,4> metrics_{};
    std::array<std::int32_t,4> clip_{};
};
}
