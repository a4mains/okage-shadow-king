#pragma once
#include "okage/assets.hpp"
#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace okage {
class GamePlayTimeCounter {
public:
    static constexpr std::uint32_t passes_per_increment=60;
    // Returns true exactly on every 60th completed pass, when the original
    // capped signed-32-bit increment is attempted at variables+0x520.
    bool main_loop_pass(std::span<std::uint8_t> variables);
private:
    std::uint32_t partial_passes_=0;
};
struct GameUnitUiRecord {
    std::array<std::uint32_t,27> words{};
    std::string name;
    std::string model_name;
};
//1254a0: zero-based BKDATA waza row; words0/12 retain asset string offsets.
struct GameActionRecord {
    std::array<std::uint32_t,13> words{};
    std::string name,description;
};
// Owned XDB records used by 0x125400/0x125360. Unit model names are copied
// from BKDATA at parse time; no guest addresses or reference snapshots enter
// this state. Actor numeric row IDs remain one-based.
class GameDataTables {
public:
    GameDataTables(Bytes battle_data,Bytes item_data);
    const std::array<std::uint32_t,27>& unit(int id) const;
    const std::string& unit_model_name(int id) const;
    const GameUnitUiRecord& ui_unit(int id) const;
    // 0x1256a8 resolves the zero-based BKDATA sysmes table (battle UI uses 0xa6).
    const std::string& system_message(unsigned index) const;
    const std::array<std::uint32_t,15>& item(int id) const;
    const std::string& item_name(int id) const;
    // Optional absent waza preserves nonbattle component fixtures; access fails.
    const GameActionRecord& action(int zero_based_id) const;
    std::size_t action_count() const {return actions_.size();}
    std::int32_t learned_skill(int unit_id,int level) const;
    std::int32_t encounter_formation(unsigned row,unsigned column) const;
    // 0x125708: BKDATA `sysval` words (0x1468c8 question-choice records),
    // terminated by 999999999. Empty when the optional section is absent.
    const std::vector<std::int32_t>& system_values() const {return system_values_;}
private:
    std::vector<std::int32_t> system_values_;
    std::vector<GameActionRecord> actions_;
    std::vector<std::string> unit_model_names_;
    std::vector<GameUnitUiRecord> ui_units_;
    std::optional<GameUnitUiRecord> preceding_ui_unit_;
    std::vector<std::string> system_messages_;
    std::vector<std::string> item_names_;
    std::vector<std::array<std::uint32_t,27>> units_;
    std::vector<std::array<std::uint32_t,15>> items_;
    std::vector<std::array<std::uint32_t,6>> learned_skills_;
    std::vector<std::array<std::uint32_t,24>> encounter_formations_;
};
void refresh_game_character(const GameDataTables& tables,
                            std::span<std::uint8_t> variables,unsigned slot);
std::array<std::uint8_t,0x52> snapshot_game_character_at_level(
    std::span<std::uint8_t> variables,const GameDataTables& tables,
    unsigned slot,std::int16_t level);
void refresh_game_characters(std::span<std::uint8_t> variables,const GameDataTables& tables,unsigned mask);
// 0x125e10: increment the three inventory counts, equip and refresh one record.
// This bounded API requires actual item IDs in the inventory range.
void grant_and_equip_game_character(std::span<std::uint8_t> variables,const GameDataTables& tables,
                                   unsigned slot,const std::array<int,3>& items);
// 0x124f10 followed by the data operations in 0x1251d8. Not a title flow/save format.
void initialize_new_game_variables(std::span<std::uint8_t> variables,const GameDataTables& tables);
}
