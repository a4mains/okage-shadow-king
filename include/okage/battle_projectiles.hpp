#pragma once
#include "okage/assets.hpp"
#include "okage/pose.hpp"
#include <cstdint>
#include <vector>

namespace okage {
struct BattleProjectile {
    std::int16_t type=0,duration=0;
    Float4 offset{};
};
class BattleProjectileData {
public:
    explicit BattleProjectileData(Bytes);
    const BattleProjectile& at_source_pointer(std::uint32_t pointer) const;
    std::size_t size() const {return records_.size();}
private:
    std::uint32_t base_=0,stride_=0;
    std::vector<BattleProjectile> records_;
};
}
