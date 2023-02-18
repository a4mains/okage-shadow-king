#include "okage/battle_entry.hpp"
#include <cstddef>
#include <algorithm>
#include <stdexcept>

namespace okage {
void clear_battle_entry_prefix(std::span<std::uint8_t> context) {
    if(context.size()<56)throw std::runtime_error("Battle entry backing extent");
    std::fill_n(context.begin(),48,0);
}
void BattleEntryContext::clear_prefix() {clear_battle_entry_prefix(storage_);}
void BattleEntryContext::clear_argument1() {storage_[0x30]=0;storage_[0x31]=0;}
void BattleEntryContext::clear_party_slots() {
    for(unsigned at:{0x26,0x2a,0x2e}){storage_[at]=0;storage_[at+1]=0;}
}

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
