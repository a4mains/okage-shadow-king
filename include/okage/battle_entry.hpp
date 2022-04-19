#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace okage {
// 0x149fd8 clears only the first 48 bytes of the separate 56-byte context.
// Validate backing first; preserve its eight parameter bytes and any suffix.
void clear_battle_entry_prefix(std::span<std::uint8_t> context);

struct BattleEntryParameters {
    std::uint16_t argument1,argument2,argument3,argument4;
};

class BattleEntryContext {
public:
    using Storage=std::array<std::uint8_t,56>;
    BattleEntryContext()=default;
    BattleEntryContext(std::uint32_t argument1,std::uint32_t argument2,
                       std::uint32_t argument3,std::uint32_t argument4);
    // Reset the entire block, then retain each argument's low 16 bits.
    void reset(std::uint32_t argument1,std::uint32_t argument2,
               std::uint32_t argument3,std::uint32_t argument4);
    // Constructor prefix reset, preserving all four previously selected args.
    void clear_prefix();
    BattleEntryParameters parameters() const;
    const Storage& bytes() const {return storage_;}
private:
    Storage storage_{};
};
}
