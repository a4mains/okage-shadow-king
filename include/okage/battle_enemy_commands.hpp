#pragma once
#include "okage/assets.hpp"
#include <array>

namespace okage {
class BattleEnemyCommands {
public:
    static constexpr unsigned rows=84, slots=7;
    explicit BattleEnemyCommands(Bytes);
    std::int16_t command(std::int16_t row,std::int16_t slot) const;
    std::uint32_t cell(std::int16_t row,std::int16_t slot) const;
private:
    std::array<std::uint32_t,rows*slots> cells_{};
};
}
