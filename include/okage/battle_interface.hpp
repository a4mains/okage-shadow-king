#pragma once
#include "okage/battle_hud.hpp"
#include "okage/ui_window.hpp"
#include "okage/battle_ui_pad.hpp"
#include "okage/battle_ui_roster.hpp"
#include "okage/battle_ui_command_state.hpp"
#include "okage/battle_ui_command_data.hpp"
#include <functional>

namespace okage {
struct BattleUiShared {
    BattleHud hud;
    std::array<std::shared_ptr<UiWindowStream>,2> streams{
        std::make_shared<UiWindowStream>(UiWindowStream{1,{}}),
        std::make_shared<UiWindowStream>(UiWindowStream{2,{}})};
    std::array<std::shared_ptr<UiWindow>,8> registry{};
    std::vector<UiWindow::AtlasTuple> atlas{UiWindow::AtlasTuple{}};
    int atlas_texture=-1;
    bool atlas_dirty=false;
    unsigned current_stream=0,stream_cursor=0;
    std::array<std::uint32_t,16> resource_accumulators{};
    BattleUiCommandState command;
    BattleUiFormationSummary formation;
    // Authored initial ELF scalars; persistent across interface construction.
    std::int32_t selected_alpha_step=-4; //1fd798; value is hud.selected_alpha.
    std::uint32_t secondary_pulse=0; //1fd7b8
    std::int32_t secondary_pulse_step=1; //1fd7d8
    std::int32_t cost_pulse=0,hp_cost_gate=0; //1fd80c,1fd804
};
struct BattleUiUpdateContext {
    std::span<std::uint8_t> variables;
    const GameDataTables& tables;
    UiPartySelection& party;
    const BattleState& root;
    const BattleData& battle_data;
    std::span<const BattleEntityBinding> entities;
    std::span<const BattleActor* const> enemies;
    unsigned game_mode;
};
class BattleInterface {
public:
    BattleInterface(BattleUiShared&,const BattleUiData&,std::shared_ptr<const void> texture,
                    std::uint32_t parent_flags,const std::function<void(unsigned)>& constructor);
    ~BattleInterface();
    BattleInterface(const BattleInterface&)=delete;
    BattleInterface& operator=(const BattleInterface&)=delete;
    const auto& words() const {return words_;}
    const auto& handles() const {return handles_;}
    const UiWindow& window(unsigned layer) const;
    BattleUiPadMasks first_update(std::shared_ptr<RuwResource>,BattleUiPad&,
                                 const BattleUiPadInput&,unsigned display_rate);
    void second_update(RuwResource&,const BattleHudData&,const BattleUiCommandData&,
                       BattleUiUpdateContext);
private:
    void release();
    BattleUiShared& shared_;
    std::array<std::uint32_t,0xef0/4> words_{};
    std::array<int,2> handles_{-1,-1};
};
}
