#include "okage/battle_hud_targets.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
template<std::size_t N> void copy(std::array<char,N>& out,std::string_view text) {
    if(text.size()>=N)throw std::runtime_error("Battle target scratch extent");
    std::copy(text.begin(),text.end(),out.begin());out[text.size()]=0;
}
template<std::size_t N> std::string_view text(const std::array<char,N>& value) {
    auto end=std::find(value.begin(),value.end(),0);
    if(end==value.end())throw std::runtime_error("Battle target scratch terminator");
    return {value.data(),std::size_t(end-value.begin())};
}
constexpr auto keep=RuwResource::preserve_component;
}
void update_battle_target_list(RuwResource& r,BattleHud& hud,const BattleHudData& data,
    const BattleUiCommandData& commands,const GameDataTables& tables,
    const BattleUiFormationSummary& formation) {
    const auto& palette=commands.target_palette();
    const auto count=std::bit_cast<std::int32_t>(formation.count);
    if(count>std::int32_t(formation.unit_ids.size()))
        throw std::runtime_error("Battle target formation extent");
    //1624e8..162538 resets exactly four visible slots, even when count is0.
    for(int row=0;row<4;++row) {
        r.set_text(3,row+0x44,commands.target_blank());
        r.set_text(3,row+0x1e,tables.system_message(0x7e));
        r.set_runtime_word0(3,row+0x19,0x47);
    }
    for(int row=0;row<count;++row) {
        r.set_runtime_word0(3,row+0x19,0x26);
        hud.copied_unit=tables.ui_unit(formation.unit_ids[unsigned(row)]); //125400
        const auto color=std::bit_cast<std::int32_t>(hud.copied_unit->words[15]);
        if(color<0||color>=std::int32_t(palette.size()))
            throw std::runtime_error("Battle target unit palette index");
        const auto rgb=palette[unsigned(color)];
        r.set_rgba(3,row+0x19,{rgb[0],rgb[1],rgb[2],keep});
        const auto& padding=tables.system_message(0x7e);
        if(padding.find('%')!=std::string::npos)
            throw std::runtime_error("Battle target padding format conversion");
        copy(hud.padded_name,padding);
        const auto& name=hud.copied_unit->name;
        if(name.size()>hud.padded_name.size())throw std::runtime_error("Battle target padded name extent");
        std::copy(name.begin(),name.end(),hud.padded_name.begin());
        copy(hud.current_number,data.number2(formation.counts[unsigned(row)]));
        copy(hud.formatted,commands.target_count_text(text(hud.current_number)));
        r.set_text(3,row+0x44,text(hud.formatted));
        copy(hud.formatted,commands.target_name_text(name));
        if(hud.type2_flag_b==1) {
            hud.type2_flag_a=1;hud.command_state_timer=0; //1fd800 /1fd7f8
            copy(hud.formatted,commands.target_blank());
        }
        r.set_text(3,row+0x1e,text(hud.formatted));
    }
}
void override_battle_target_display(RuwResource& r,const BattleUiCommandData& commands,
    const GameDataTables& tables,std::int32_t category,std::int32_t option,
    std::int32_t available,bool selected_action) {
    const auto add=[&](std::int32_t id){r.add_rgba(3,id,{keep,keep,keep,128});};
    const auto clear_counts=[&] {
        r.set_text(3,0x1e,tables.system_message(0x85));
        for(int row=0;row<4;++row)r.set_text(3,row+0x44,commands.target_blank());
    };
    if(available<2&&selected_action&&category==0&&option==1) {
        add(0x1e);clear_counts();
    }
    if(option==2) {
        add(0x1e);add(0x27);add(0x1e);clear_counts();return;
    }
    //Source ADDIU wraps signed words; RUW lookup rejects unowned resulting IDs.
    add(std::bit_cast<std::int32_t>(std::uint32_t(option)+2));
    add(std::bit_cast<std::int32_t>(std::uint32_t(option)+14));
}
}
