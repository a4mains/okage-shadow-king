#pragma once
#include "okage/assets.hpp"
#include <array>
#include <span>
#include <vector>

namespace okage {
// Owned XDB numeric records used by 0x125400/0x125360. Text offsets remain
// asset offsets; no guest addresses or reference snapshots enter this state.
class GameDataTables {
public:
    GameDataTables(Bytes battle_data,Bytes item_data);
    const std::array<std::uint32_t,27>& unit(int id) const;
    const std::array<std::uint32_t,15>& item(int id) const;
private:
    std::vector<std::array<std::uint32_t,27>> units_;
    std::vector<std::array<std::uint32_t,15>> items_;
};
void refresh_game_characters(std::span<std::uint8_t> variables,const GameDataTables& tables,unsigned mask);
// 0x125e10: increment the three inventory counts, equip and refresh one record.
// This bounded API requires actual item IDs in the inventory range.
void grant_and_equip_game_character(std::span<std::uint8_t> variables,const GameDataTables& tables,
                                   unsigned slot,const std::array<int,3>& items);
// 0x124f10 followed by the data operations in 0x1251d8. Not a title flow/save format.
void initialize_new_game_variables(std::span<std::uint8_t> variables,const GameDataTables& tables);
}
