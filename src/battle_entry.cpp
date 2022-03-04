#include "okage/battle_entry.hpp"
#include <cstddef>

namespace okage {
BattleEntryContext::BattleEntryContext(std::uint32_t a,std::uint32_t b,
                                     std::uint32_t c,std::uint32_t d) {
    reset(a,b,c,d);
}
void BattleEntryContext::reset(std::uint32_t a,std::uint32_t b,
                              std::uint32_t c,std::uint32_t d) {
    // 0x126800 calls the zero-fill wrapper 0x1bd6b8 with length 0x38.
    storage_.fill(0);
    const auto put=[&](std::size_t offset,std::uint32_t value) {
        storage_[offset]=static_cast<std::uint8_t>(value);
        storage_[offset+1]=static_cast<std::uint8_t>(value>>8);
    };
    // Preserve the original SH order; all argument bits above bit 15 are lost.
    put(0x36,d);
    put(0x30,a);
    put(0x32,b);
    put(0x34,c);
}
BattleEntryParameters BattleEntryContext::parameters() const {
    const auto get=[&](std::size_t offset) {
        return static_cast<std::uint16_t>(std::uint16_t(storage_[offset]) |
                                         (std::uint16_t(storage_[offset+1])<<8));
    };
    return {get(0x30),get(0x32),get(0x34),get(0x36)};
}
}
