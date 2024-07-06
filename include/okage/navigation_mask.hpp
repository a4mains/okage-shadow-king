#pragma once
#include "okage/assets.hpp"
#include <array>
#include <cstdint>

namespace okage {
class NavigationMask {
public:
    NavigationMask(std::uint32_t texture_slot, unsigned width, unsigned height);
    std::uint32_t texture_slot() const { return texture_slot_; }
    Bytes bytes() const { return bytes_; }
    // 0x1148b8. Source upper-Y clamp changes X, not Y. Unsafe resulting
    // source writes are rejected before mutation, not silently repaired.
    void reveal_cell(std::int32_t x, std::int32_t y, std::uint8_t value=0xff);
    // 0x124ef0/0x124ed0: variables+0x878+index*0x80. Native supports the ten
    // complete records in the0xd78 backing; original helpers lack this guard.
    void restore_record(Bytes variables, std::int32_t index);
    void pack_record(std::span<std::uint8_t> variables, std::int32_t index) const;
private:
    std::uint32_t texture_slot_;
    std::array<std::uint8_t,1024> bytes_{};
};
}
