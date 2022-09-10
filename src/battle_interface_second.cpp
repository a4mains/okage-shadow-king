#include "okage/battle_interface.hpp"
#include "okage/battle_actor.hpp"
#include <bit>
#include <limits>
#include <stdexcept>

namespace okage {
namespace {
constexpr auto keep=RuwResource::preserve_component;
std::int32_t signed_word(std::uint32_t v){return __builtin_bit_cast(std::int32_t, (v));}
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
            case 4:case 19:
                h.reset_type2_widgets(r,data,context.tables,context.variables,context.party,context.game_mode,selected);
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
            default:throw std::runtime_error("Battle UI menu branch unrecovered: "+std::to_string(signed_word(h.menu_state)));
            }
            if(battle_ui_available_party(context.entities)<2&&selected_action&&category==0) {
                r.set_runtime_word0(3,15,77);r.set_runtime_word0(3,46,77);
            } else if(category==3) {
                r.set_runtime_word0(3,14,79);r.set_runtime_word0(3,15,20);r.set_runtime_word0(3,46,6);
            } else {
                r.set_runtime_word0(3,15,19);r.set_runtime_word0(3,46,77);
            }
            // The reached menu0 body. Preserve add/clamp semantics and order.
            r.add_rgba(2,55,{keep,keep,keep,4});
            r.add_extent(2,10,323,keep,323,keep);r.add_position(2,10,291,keep,291,keep);
            for(int id:{11,12,13,24})alpha(r,2,id,127);
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
