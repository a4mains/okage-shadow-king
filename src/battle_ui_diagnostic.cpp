#include "okage/battle_ui_diagnostic.hpp"
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace okage {
std::string battle_ui_update_snapshot(const BattleInterface& ui,const BattleUiShared& s,
    const RuwResource& r,Bytes variables,const UiPartySelection& party,bool scheduled) {
    std::ostringstream out;
    const auto array=[&](const auto& values){out<<'[';bool comma=false;for(auto v:values){if(comma)out<<',';comma=true;out<<v;}out<<']';};
    const auto hex=[&](const auto& bytes){out<<'"';constexpr char d[]="0123456789abcdef";for(auto v:bytes){const auto u=std::uint8_t(v);out<<d[u>>4]<<d[u&15];}out<<'"';};
    const auto buffer=[&](const UiWindowBuffer& b){std::vector<std::uint8_t> bytes;for(std::size_t i=0;i<b.size();++i){auto v=b.known_byte(i);if(!v)throw std::runtime_error("Unknown UI diagnostic glyph byte");bytes.push_back(*v);}hex(bytes);};
    out<<"{\"manager_words\":";array(ui.words());out<<",\"windows\":[";
    for(unsigned layer=0;layer<2;++layer) {
        if(layer)out<<',';const auto& w=ui.window(layer);
        out<<"{\"slot\":"<<ui.handles()[layer]<<",\"scalar_words\":[";
        for(unsigned at=0;at<UiWindow::scalar_bytes;at+=4){if(at)out<<',';if(auto v=w.known_word(at))out<<*v;else out<<"null";}
        out<<"],\"commands\":[";bool comma=false;
        for(const auto& entry:w.stream().entries) {
            if(comma)out<<',';comma=true;const auto& cmd=std::get<UiStreamCommand>(entry);
            out<<"{\"opcode\":"<<cmd.opcode;
            if(const auto* ref=std::get_if<UiWidgetReference>(&cmd.argument)) {
                if(ref->resource.get()!=&r)throw std::runtime_error("UI diagnostic widget owner");
                out<<",\"group\":"<<ref->group<<",\"index\":"<<ref->index;
            } else if(const auto* ref=std::get_if<UiStreamReference>(&cmd.argument)) {
                const char* binding=*ref==UiStreamReference::RuwMetrics200b78?"ruw_metrics":
                    *ref==UiStreamReference::RuwClip200b88?"ruw_clip":nullptr;
                if(!binding)throw std::runtime_error("UI diagnostic setup command reference");
                out<<",\"binding\":"<<std::quoted(binding);
            } else throw std::runtime_error("UI diagnostic unexpected command argument");
            out<<'}';
        }
        out<<"],\"glyph_positions_hex\":";buffer(w.glyph_buffer(0));
        out<<",\"glyph_metrics_hex\":";buffer(w.glyph_buffer(1));out<<'}';
    }
    out<<"],\"hud\":"<<s.hud.diagnostic(r,variables,scheduled?"scheduled-ui-update":"ui-update");
    out<<",\"party_counts\":";array(party.counts());out<<",\"party_rows\":[";
    for(unsigned i=0;i<party.rows().size();++i){if(i)out<<',';array(party.rows()[i]);}out<<']';
    out<<",\"resource_accumulators\":";array(s.resource_accumulators);
    out<<",\"command_changed\":";array(s.command.changed);
    out<<",\"command_values\":";array(s.command.values);
    out<<",\"command\":"<<s.command.command<<",\"selected_alpha\":"<<s.hud.selected_alpha
       <<",\"selected_alpha_step\":"<<s.selected_alpha_step<<",\"secondary_pulse\":"<<s.secondary_pulse
       <<",\"secondary_pulse_step\":"<<s.secondary_pulse_step<<",\"cost_pulse\":"<<s.cost_pulse
       <<",\"hp_cost_gate\":"<<s.hp_cost_gate<<",\"type3_blink\":"<<s.hud.type3_blink;
    out<<",\"type3_formatted_hex\":";hex(s.hud.type3_formatted);
    out<<",\"type3_padded_name_hex\":";hex(s.hud.type3_padded_name);
    out<<",\"type3_current_number_hex\":";hex(s.hud.type3_current_number);
    out<<",\"type3_maximum_number_hex\":";hex(s.hud.type3_maximum_number);
    out<<",\"formation_count\":"<<s.formation.count<<",\"formation_unit_ids\":";array(s.formation.unit_ids);
    out<<",\"formation_counts\":";array(s.formation.counts);out<<",\"formation_groups\":";array(s.formation.groups);
    out<<'}';return out.str();
}
}
