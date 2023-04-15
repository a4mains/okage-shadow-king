#pragma once
#include "okage/assets.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace okage {
// Authored 0x88-byte descriptor, kept as data, never guest code or pointers.
// Consumers must recover each field they use; no animation execution implied.
struct BattleActionDescriptor {
    std::array<std::uint8_t,0x88> bytes{};
    std::int16_t half(std::size_t offset) const;
};
struct BattleSecondaryActionSelection {
    const BattleActionDescriptor& descriptor;
    std::int16_t type;
};
class BattleActionData {
public:
    explicit BattleActionData(Bytes);
    const BattleActionDescriptor& motion(std::int32_t action,std::int32_t actor_kind,
                                          std::int32_t source_identity) const;
    BattleSecondaryActionSelection secondary(std::int32_t action,std::int32_t mode) const;
    std::size_t action_count() const {return action_map_.size();}
private:
    std::vector<std::int16_t> unit_map_,action_map_;
    std::vector<BattleActionDescriptor> paired_,generic_,secondary_;
};
}
