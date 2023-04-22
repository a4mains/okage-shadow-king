#include "okage/battle_interface.hpp"
#include "okage/battle_actor.hpp"
#include "okage/battle_hud_targets.hpp"
#include <bit>
#include <limits>
#include <stdexcept>

namespace okage {
namespace {
constexpr auto keep=RuwResource::preserve_component;
std::int32_t signed_word(std::uint32_t v){return std::bit_cast<std::int32_t>(v);}
std::uint32_t word(Bytes b,unsigned at) {
    if(at>b.size()||b.size()-at<4)throw std::runtime_error("Battle UI state word extent");
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
void pulse(std::uint32_t& value,std::int32_t& step,int low,int high,bool lower_first) {
    value+=std::uint32_t(step);
    const auto lower=[&]{if(signed_word(value)<low){value=std::uint32_t(low);step=4;}};
    const auto upper=[&]{if(signed_word(value)>high){value=std::uint32_t(high);step=-4;}};
    if(lower_first){lower();upper();}else{upper();lower();}
}
void alpha(RuwResource& r,unsigned group,int id,std::int32_t a){r.set_rgba(group,id,{keep,keep,keep,a});}
}
void BattleInterface::second_update(RuwResource& r,const BattleHudData& data,
    const BattleUiCommandData& commands,BattleUiUpdateContext context) {
    for(unsigned layer=0;layer<words_[0xe40/4];++layer) {
        if(layer>=handles_.size())throw std::runtime_error("Battle interface second layer extent");
        const unsigned base=(0x40+layer*0xe0)/4;
        if(!(words_[base+0x50/4]&0x40)||!words_[base+0x5c/4])continue;
        const auto handle=handles_[layer];
        if(handle<0||unsigned(handle)>=shared_.registry.size()||!shared_.registry[unsigned(handle)])
            throw std::runtime_error("Battle interface second window lifetime");
        auto& w=*shared_.registry[unsigned(handle)];
        auto& h=shared_.hud;
        switch(words_[base+0x5c/4]) {
        case 1: {
            shared_.formation.refresh(context.root,context.battle_data,context.enemies,context.game_mode);
            const int selected=context.game_mode==4?0:signed_word(word(context.root.bytes,0x81ac));
            bool selected_action=false;
            if(context.game_mode!=4) {
                if(selected<0||std::size_t(selected)>=context.entities.size())
                    throw std::runtime_error("Battle UI selected entity extent");
                if(const auto* actor=context.entities[unsigned(selected)].actor)
                    selected_action=word(actor->storage,0x17c)!=0;
            }
            pulse(h.selected_alpha,shared_.selected_alpha_step,40,128,true);
            w.set_transform_parameter(16);
            h.refresh(r,data,context.tables,context.variables,context.party,context.game_mode,selected);
            auto layer_words=std::span(words_).subspan(base,0xe0/4);
            shared_.command.import_layer(layer_words,h.menu_state,h.selection_state,w);
            BattleUiCommandState::move_layer_y(layer_words,h.menu_state,w);
            context.party.refresh(context.variables,context.game_mode);
            const int category=signed_word(shared_.command.values[0]);
            if(category==3&&shared_.command.values[1]==2&&h.menu_state==16)
                throw std::runtime_error("Battle UI special resource-cost action unrecovered");
            h.refresh_command_costs(r,data,context.tables,context.variables,context.party,
                context.game_mode,selected,0,1,shared_.cost_pulse,shared_.hp_cost_gate);
            pulse(shared_.secondary_pulse,shared_.secondary_pulse_step,80,255,false);
            const auto& icons=category==0?commands.category_zero_commands():commands.category_nonzero_commands();
            r.set_runtime_word0(3,14,std::uint32_t(icons[0]));
            r.set_runtime_word0(3,15,std::uint32_t(icons[1]));
            switch(signed_word(shared_.command.command)) {
            case 0:case 17:break;
            case 1:
                h.display_command_state_one(r);
                break;
            case 2: {
                const auto panel_y=category==0?0x144:0x142;
                r.set_position(3,5,0x8d,panel_y);
                r.add_extent(3,5,-0x448,-0x448,0x60,0x14);
                update_battle_target_list(r,h,data,commands,context.tables,shared_.formation);
                if(h.menu_state==2)
                    h.position_target_menu_two(r,signed_word(shared_.command.values[1]),
                        context.variables[0x0c],category,selected_action,
                        battle_ui_available_party(context.entities));
                break;
            }
            case 4:case 19:
                h.reset_type2_widgets(r,data,context.tables,context.variables,context.party,context.game_mode,selected);
                break;
            case 5:
                //164090 command5: update target labels before the one-shot
                //161c88 confirmation-panel setup.
                if(category<0||category>3)
                    throw std::runtime_error("Battle target confirmation category extent");
                update_battle_target_list(r,h,data,commands,context.tables,shared_.formation);
                h.begin_target_confirm(r,category);
                break;
            case -1:break;
            default:throw std::runtime_error("Battle UI command branch unrecovered: "+
                std::to_string(signed_word(shared_.command.command)));
            }
            if(shared_.command.command!=0xffffffffu&&!h.selection_state)h.selection_state=1;
            if(category<0||std::size_t(category)>=commands.selection_commands().size())
                throw std::runtime_error("Battle UI category outside authored selection table");
            const auto pair=commands.selection_commands()[unsigned(category)];
            if(pair==std::numeric_limits<std::int32_t>::max())throw std::runtime_error("Battle UI pair ID extent");
            alpha(r,2,pair,128);alpha(r,2,pair+1,128);
            switch(h.menu_state) {
            case 0:
                h.refresh_command_party(r,data,context.tables,context.variables,context.party,context.game_mode,
                    selected,0,std::int32_t(battle_ui_available_party(context.entities)),h.selection_state,
                    signed_word(h.selected_alpha));
                r.select_class_color(2,0,category);r.select_class_color(2,1,category);
                alpha(r,2,pair,signed_word(h.selected_alpha));alpha(r,2,pair+1,signed_word(h.selected_alpha));
                break;
            case 1: {
                if(!context.entry)throw std::runtime_error("Battle target menu1 requires the owned entry context");
                h.command_state_timer=0;
                h.type2_flag_a=0;
                const auto available=battle_ui_available_party(context.entities);
                h.refresh_command_party(r,data,context.tables,context.variables,context.party,
                    context.game_mode,selected,1,std::int32_t(available),h.selection_state,
                    signed_word(h.selected_alpha));
                const auto target=signed_word(shared_.command.values[1]);
                r.select_class_color(3,1,target);r.select_class_color(3,2,target);
                const auto& entry=context.entry->bytes();
                const auto bank7_bits=std::uint16_t(std::uint16_t(entry[0x36])|
                    (std::uint16_t(entry[0x37])<<8));
                const auto bank7=std::bit_cast<std::int16_t>(bank7_bits);
                h.update_target_menu_one(r,context.variables,context.game_mode,category,
                    selected_action,available,context.variables[0x0c],bank7);
                break;
            }
            case 2: {
                if(context.game_mode!=3)
                    throw std::runtime_error("Battle target menu2 supports only the reached mode3 path");
                const auto option=signed_word(shared_.command.values[1]);
                const auto target_row=signed_word(shared_.command.values[2]);
                if(target_row<0||target_row>3)
                    throw std::runtime_error("Battle target menu2 row extent");
                r.set_position(3,21,keep,target_row*0x12+0x14a);
                r.set_position(3,65,keep,target_row*0x12+0x14a);
                if(h.update_target_menu_two_motion(r,option)!=0)
                    (void)h.update_target_menu_two_rows(r,category);
                h.update_target_menu_selector(r);
                h.update_target_menu_two_animation(r,category);
                update_battle_target_list(r,h,data,commands,context.tables,shared_.formation);
                r.add_class_rgba(3,3,{keep,keep,keep,4});
                r.add_rgba(2,51,{keep,keep,keep,-16});
                override_battle_target_display(r,commands,context.tables,category,option,
                    std::int32_t(battle_ui_available_party(context.entities)),selected_action);
                break;
            }
            case 4:
                break;
            case 5: {
                const auto selected_row=signed_word(shared_.command.values[2]);
                if(selected_row<0||selected_row>3)
                    throw std::runtime_error("Battle target confirmation row extent");
                r.add_position(4,2,-0x20,keep,0x80,keep);
                r.add_extent(4,2,-0x20,-8,0x78,0x13);
                const int row_selector=signed_word(shared_.command.values[1]);
                if(row_selector<0||row_selector>2)
                    throw std::runtime_error("Battle target confirmation selector extent");
                const int bounded=row_selector<0?row_selector+7:row_selector;
                const int id=row_selector-(bounded>>3)*8+0x1b;
                r.add_position(4,id,-0x20,-8,0x92,0x14a);
                if(h.update_target_menu_two_motion(r,row_selector)!=0)
                    r.add_class_rgba(3,0,{keep,keep,keep,4});
                h.update_target_menu_selector(r);
                h.update_target_menu_two_animation(r,category);
                alpha(r,2,51,0);r.add_rgba(2,51,{keep,keep,keep,-16});
                h.update_target_confirm(r,category,selected_row);
                r.add_rgba(3,7,{keep,keep,keep,128});
                // FUN_001620b8 has a one-instruction no-op body.
                override_battle_target_display(r,commands,context.tables,category,
                    row_selector,std::int32_t(battle_ui_available_party(context.entities)),selected_action);
                h.clear_target_confirm_cursor(r);
                break;
            }
            default:throw std::runtime_error("Battle UI menu branch unrecovered: "+std::to_string(signed_word(h.menu_state)));
            }
            if(battle_ui_available_party(context.entities)<2&&selected_action&&category==0) {
                r.set_runtime_word0(3,15,77);r.set_runtime_word0(3,46,77);
            } else if(category==3) {
                r.set_runtime_word0(3,14,79);r.set_runtime_word0(3,15,20);r.set_runtime_word0(3,46,6);
            } else {
                r.set_runtime_word0(3,15,19);r.set_runtime_word0(3,46,77);
            }
            if(h.menu_state==0) {
                r.add_rgba(2,55,{keep,keep,keep,4});
                r.add_extent(2,10,0x143,keep,0x143,keep);
                r.add_position(2,10,0x123,keep,0x123,keep);
                for(int id:{11,12,13,24})alpha(r,2,id,127);
            } else if(h.menu_state!=0) {
                r.add_rgba(2,55,{keep,keep,keep,-128});
                r.add_extent(2,10,0x143,keep,0xf3,keep);
                r.add_position(2,10,0x123,keep,0x173,keep);
            }
            if(category==0||category==3) {
                r.set_position(3,12,252,332);r.set_position(3,36,135,333);
                r.add_extent(3,12,20,keep,20,keep);r.add_extent(4,1,22,keep,22,keep);
            } else {
                r.set_position(3,12,260,332);
                r.add_extent(3,12,20,keep,20,keep);r.add_extent(3,12,12,keep,12,keep);
                r.add_extent(4,1,6,keep,6,keep);
            }
            alpha(r,2,15,127);
            break;
        }
        case 2:
            h.update_type3(r,data,context.tables,context.variables,context.party,context.game_mode,
                [&](bool enabled){w.set_layout_enabled(enabled);});
            break;
        default:throw std::runtime_error("Battle interface second callback binding unrecovered");
        }
    }
}
}
