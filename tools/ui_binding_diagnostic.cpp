#include "okage/battle_interface.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {
std::vector<std::uint8_t> read(const char* path) {
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("Missing owned UI binding input");
    return {(std::istreambuf_iterator<char>(in)),{}};
}
void buffer(std::ostream& out,const okage::UiWindowBuffer& b) {
    constexpr char digits[]="0123456789abcdef";out<<'"';
    for(std::size_t i=0;i<b.size();++i) {
        const auto v=b.known_byte(i);
        if(!v)throw std::runtime_error("Unknown bound glyph output byte");
        out<<digits[*v>>4]<<digits[*v&15];
    }
    out<<'"';
}
}
int main(int argc,char** argv) {try {
    if(argc!=8)throw std::runtime_error("usage: okage-ui-binding HUD BKDATA ITEM RUW NATIVE_VARIABLES UI FRESH_JSON");
    const okage::BattleHudData hud_data(read(argv[1]));
    const okage::GameDataTables tables(read(argv[2]),read(argv[3]));
    auto ruw=std::make_shared<okage::RuwResource>(read(argv[4]));
    const auto vars=read(argv[5]);const okage::BattleUiData data(read(argv[6]));
    if(vars.size()!=4096)throw std::runtime_error("UI binding diagnostic requires owned native variable extent");
    if(std::filesystem::exists(argv[7])||std::filesystem::is_symlink(argv[7]))
        throw std::runtime_error("Refusing existing UI binding diagnostic output");
    okage::UiPartySelection party;okage::BattleUiShared shared;
    okage::BattleInterface interface(shared,data,std::make_shared<int>(1),0,[&](unsigned type){
        if(type==2)shared.hud.initialize_type2(*ruw,hud_data,tables,vars,party,3,0);
        else if(type==3)shared.hud.initialize_type3(*ruw,tables);
        else throw std::runtime_error("Unsupported binding diagnostic layer");
    });
    const std::array<std::size_t,3> type2{2,3,4};
    const std::array<std::size_t,1> type3{5};
    shared.registry.at(interface.handles()[0])->bind_ruw(ruw,type2);
    shared.registry.at(interface.handles()[1])->bind_ruw(ruw,type3);
    std::ofstream out(argv[7]);
    out<<"{\"schema\":\"okage-native-ui-binding-v1\",\"scope\":\"owned constructors followed only by RUW binding/reset; no UI callbacks or resource clocks\",\"windows\":[";
    for(unsigned layer=0;layer<2;++layer) {
        if(layer)out<<',';const auto& w=interface.window(layer);
        out<<"{\"slot\":"<<interface.handles()[layer]<<",\"scalar_words\":[";
        for(unsigned at=0;at<okage::UiWindow::scalar_bytes;at+=4) {
            if(at)out<<',';if(auto v=w.known_word(at))out<<*v;else out<<"null";
        }
        out<<"],\"commands\":[";bool comma=false;
        for(const auto& entry:w.stream().entries) {
            if(comma)out<<',';comma=true;const auto& command=std::get<okage::UiStreamCommand>(entry);
            out<<"{\"opcode\":"<<command.opcode;
            if(const auto* ref=std::get_if<okage::UiWidgetReference>(&command.argument)) {
                if(ref->resource.get()!=ruw.get())throw std::runtime_error("Wrong native RUW stream owner");
                out<<",\"group\":"<<ref->group<<",\"index\":"<<ref->index;
            } else if(const auto* ref=std::get_if<okage::UiStreamReference>(&command.argument)) {
                const char* binding=*ref==okage::UiStreamReference::RuwMetrics200b78?"ruw_metrics":
                    *ref==okage::UiStreamReference::RuwClip200b88?"ruw_clip":nullptr;
                if(!binding)throw std::runtime_error("Unexpected native setup command reference");
                out<<",\"binding\":"<<std::quoted(binding);
            } else throw std::runtime_error("Unexpected native binding scalar argument");
            out<<'}';
        }
        out<<"],\"glyph_positions_hex\":";buffer(out,w.glyph_buffer(0));
        out<<",\"glyph_metrics_hex\":";buffer(out,w.glyph_buffer(1));out<<'}';
    }
    out<<"],\"hud\":"<<shared.hud.diagnostic(*ruw,vars,"isolated")<<"}\n";
    if(!out)throw std::runtime_error("Cannot write native UI binding diagnostic");
    std::cout<<"Owned UI binding diagnostic complete\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
