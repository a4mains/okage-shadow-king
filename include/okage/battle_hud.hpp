#pragma once
#include "okage/battle_ui.hpp"
#include "okage/game_state.hpp"
#include "okage/ruw.hpp"
#include <string>

namespace okage {
class BattleHudData {
public:
    explicit BattleHudData(Bytes);
    const auto& palette() const {return palette_;}
    double warning_fraction() const {return warning_fraction_;}
    std::string number(std::int32_t value) const;
private:
    std::array<std::array<std::int32_t,3>,6> palette_{};
    double warning_fraction_{};
    std::array<std::string,3> formats_;
};
struct BattleHud {
    std::uint32_t menu_state=0,mode_counter=0,selection_state=0,selected_alpha=128;
    bool bank2_bound=false;
    std::uint32_t type2_flag_a=0,type2_flag_b=0,type2_enabled=1;
    std::array<std::uint32_t,4> row_flags{};
    std::uint32_t flag_a0=0,flag_a8=0;
    std::array<std::uint32_t,512> type2_work{};
    std::uint32_t type3_ready=0,type3_reset=0,type3_animation=0,type3_cursor=0;
    std::array<std::int32_t,15> type3_slots{};
    std::array<char,256> formatted{},padded_name{};
    std::array<char,64> current_number{},maximum_number{};
    std::optional<GameUnitUiRecord> copied_unit;

    void refresh(RuwResource&,const BattleHudData&,const GameDataTables&,
                 Bytes variables,const UiPartySelection&,unsigned game_mode,int selected);
    void initialize_type2(RuwResource&,const BattleHudData&,const GameDataTables&,
                          Bytes variables,const UiPartySelection&,unsigned game_mode,int selected);
    void reset_type3(RuwResource&);
    void initialize_type3(RuwResource&,const GameDataTables&);
    std::string diagnostic(const RuwResource&,Bytes variables,std::string_view execution) const;
};
}
