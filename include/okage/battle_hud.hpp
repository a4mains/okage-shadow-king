#pragma once
#include "okage/battle_ui.hpp"
#include "okage/game_state.hpp"
#include "okage/ruw.hpp"
#include <functional>
#include <string>

namespace okage {
class BattleHudData {
public:
    explicit BattleHudData(Bytes);
    const auto& palette() const {return palette_;}
    double warning_fraction() const {return warning_fraction_;}
    std::string number(std::int32_t value) const;
    // 124200's two-digit resource formatter requires the v2 owned import.
    std::string number2(std::int32_t value) const;
private:
    std::array<std::array<std::int32_t,3>,6> palette_{};
    double warning_fraction_{};
    std::array<std::string,3> formats_;
    std::optional<std::array<std::string,2>> short_formats_;
};
struct BattleHud {
    std::uint32_t menu_state=0,mode_counter=0,selected_alpha=128;
    std::int32_t command_state_timer=0; //1fd7f8; independent of command1fd860.
    std::int32_t selection_state=0;
    bool bank2_bound=false;
    std::uint32_t type2_flag_a=0,type2_flag_b=0,type2_enabled=1;
    std::array<std::uint32_t,4> row_flags{};
    std::uint32_t flag_a0=0,flag_a8=0;
    std::uint32_t target_pulse=0;std::int32_t target_pulse_step=1; //1fd7bc/1fd7dc
    std::array<std::uint32_t,512> type2_work{};
    std::uint32_t type3_ready=0,type3_reset=0,type3_animation=0,type3_cursor=0;
    std::uint32_t type3_blink=0;
    std::array<std::int32_t,15> type3_slots{};
    std::array<char,256> formatted{},padded_name{};
    std::array<char,64> current_number{},maximum_number{};
    std::array<char,256> type3_formatted{},type3_padded_name{};
    std::array<char,64> type3_current_number{},type3_maximum_number{};
    std::optional<GameUnitUiRecord> copied_unit;

    void refresh(RuwResource&,const BattleHudData&,const GameDataTables&,
                 Bytes variables,const UiPartySelection&,unsigned game_mode,int selected);
    void initialize_type2(RuwResource&,const BattleHudData&,const GameDataTables&,
                          Bytes variables,const UiPartySelection&,unsigned game_mode,int selected);
    // 15e678 alone; constructor-only pre/post writes remain in initialize_type2.
    void reset_type2_widgets(RuwResource&,const BattleHudData&,const GameDataTables&,
                            Bytes,const UiPartySelection&,unsigned game_mode,int selected);
    void refresh_command_costs(RuwResource&,const BattleHudData&,const GameDataTables&,
        Bytes,const UiPartySelection&,unsigned game_mode,int selected_row3,
        std::int32_t cost,std::int32_t kind,std::int32_t pulse_counter,std::int32_t hp_cost_gate);
    void refresh_command_party(RuwResource&,const BattleHudData&,const GameDataTables&,
        Bytes,const UiPartySelection&,unsigned game_mode,int selected_row2,
        std::int32_t display_mode,std::int32_t available_count,
        std::int32_t& command_countdown,std::int32_t command_alpha);
    //164090 command/state1: its unconditional widget prelude (1626d8).
    void display_command_state_one(RuwResource&);
    void update_target_menu_one(RuwResource&,Bytes variables,unsigned game_mode,
        int category,bool selected_action,unsigned available_count,
        std::uint8_t bank1_mode,std::int16_t bank7_state);
    void update_target_menu_selector(RuwResource&);
    std::uint32_t update_target_menu_two_motion(RuwResource&,int option);
    bool update_target_menu_two_rows(RuwResource&,int category);
    void update_target_menu_two_animation(RuwResource&,int category);
    void position_target_menu_two(RuwResource&,int option,std::uint8_t bank1_mode,
        int category,bool selected_action,unsigned available_count);
    void begin_target_confirm(RuwResource&,int category);
    void update_target_confirm(RuwResource&,int category,int selected_row);
    void clear_target_confirm_cursor(RuwResource&);
    void reset_type3(RuwResource&);
    void initialize_type3(RuwResource&,const GameDataTables&);
    void refresh_type3(RuwResource&,const BattleHudData&,const GameDataTables&,
                       Bytes variables,const UiPartySelection&);
    void update_type3(RuwResource&,const BattleHudData&,const GameDataTables&,
                      Bytes variables,const UiPartySelection&,unsigned game_mode,
                      const std::function<void(bool)>& set_layout_enabled);
    std::string diagnostic(const RuwResource&,Bytes variables,std::string_view execution) const;
};
}
