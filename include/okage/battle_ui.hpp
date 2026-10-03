#pragma once
#include "okage/assets.hpp"
#include <array>

namespace okage {
struct BattleUiDefinition {
    std::array<std::uint32_t,28> words{};
    unsigned stream() const {return words[0];}
    unsigned type() const {return words[24];}
    unsigned glyph_capacity() const {return words[25];}
    unsigned sprite_capacity() const {return words[26];}
};
class BattleUiData {
public:
    explicit BattleUiData(Bytes);
    const std::array<BattleUiDefinition,2>& layers() const {return layers_;}
private:
    std::array<BattleUiDefinition,2> layers_;
};
class UiPartySelection {
public:
    void refresh(std::span<std::uint8_t> variables,unsigned game_mode);
    std::int32_t at(unsigned row,unsigned column) const;
    const auto& counts() const {return counts_;}
    const auto& rows() const {return rows_;}
private:
    // Owned ELF initial values are zero; a real refresh fills unused slots -1.
    std::array<std::uint32_t,8> counts_{};
    std::array<std::array<std::int32_t,8>,8> rows_{};
};
bool first_battle_zero_selection(std::int32_t selected,const UiPartySelection& party);
bool first_battle_diagnostics_omitted(bool stage_requested,bool ui_update_requested,bool stage_committed,
                                      std::int32_t selected,const UiPartySelection& party);
}
