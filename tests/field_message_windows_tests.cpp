#include "okage/field_message_windows.hpp"
#include "okage/battle_hud.hpp"
#include "okage/game_state.hpp"
#include "okage/results_text.hpp"
#include "okage/rng_site.hpp"
#include "okage/scene_resource.hpp"
#include "okage/pose.hpp"
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fs=std::filesystem;
namespace {
void require(bool value,const std::string& why) {if(!value)throw std::runtime_error(why);}
template<class F> void rejects(F f,const char* why) {
    try{f();}catch(const std::runtime_error&){return;}
    throw std::runtime_error(why);
}
std::vector<std::uint8_t> read(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("missing owned input "+path.string());
    return {std::istreambuf_iterator<char>(in),{}};
}
using Site=okage::RngSite;
std::vector<Site> pass(unsigned glyphs,unsigned icons) {
    std::vector<Site> out(glyphs,Site::UiGlyph);
    for(unsigned i=0;i<icons;++i){out.push_back(Site::UiIcon);out.push_back(Site::UiSelect);}
    return out;
}
std::string runs(const std::vector<Site>& v) {
    std::string out;std::size_t i=0;
    while(i<v.size()){std::size_t j=i;while(j<v.size()&&v[j]==v[i])++j;
        out+=std::to_string(unsigned(v[i]))+"x"+std::to_string(j-i)+" ";i=j;}
    return out;
}
std::vector<Site> join(std::vector<Site> a,const std::vector<Site>& b){a.insert(a.end(),b.begin(),b.end());return a;}
std::string text(const okage::RuwResource& r,std::size_t group,std::int32_t id) {
    const auto& w=r.widget(group,id);
    return std::string(w.runtime->text->data());
}
void owned(const fs::path& stage) {
    const auto ui_dir=stage/"NATIVE_UI";
    const okage::FieldMessageWindowData data(read(ui_dir/"message-windows.bin"));
    require(data.row(0)[24]==0&&data.row(1)[24]==1&&data.row(0)[25]==16&&data.row(1)[25]==12,"OKMSW1 rows");
    auto bad=read(ui_dir/"message-windows.bin");bad[88+24*4]^=1; //row A type word
    rejects([&]{okage::FieldMessageWindowData d(bad);},"changed row accepted");
    const okage::GameDataTables tables(read(stage/"CMNDATA/XDB/BKDATA.XDB"),read(stage/"CMNDATA/XDB/ITEM.XDB"));
    const okage::ResultsTextData money(read(ui_dir/"results-text.bin"));
    const okage::BattleHudData numbers(read(ui_dir/"battle-hud.bin"));
    auto fonts=std::make_shared<const std::vector<okage::Font>>(std::vector<okage::Font>{
        okage::read_fnt(read(stage/"CMNDATA/FONT/IQ24.FNT")),okage::read_fnt(read(stage/"CMNDATA/FONT/BMUI.FNT")),
        okage::read_fnt(read(stage/"CMNDATA/FONT/BM.FNT"))});
    auto ruw=std::make_shared<okage::RuwResource>(read(ui_dir/"FLD.RUW"));

    okage::BattleUiShared ui;okage::initialize_scene_ui(ui);
    okage::GameRandom random{0x13579bdu};
    std::vector<Site> sites;
    random.observe=[](void* c,std::uint32_t){static_cast<std::vector<Site>*>(c)->push_back(okage::current_rng_site);};
    random.observe_context=&sites;
    const auto sine=okage::make_sine_table();
    okage::FieldMessageGlobals globals;
    //State left by earlier messages in the same field (original RAM): a
    //speaker was shown, and 146ee8 had faded choice rows 18..20 to alpha 0
    //(they load at their authored 0x80 and lose 32 per traversal).
    globals.last_speaker="Linda";
    for(int id=18;id<=20;++id)ruw->set_rgba(0,id,{okage::RuwResource::preserve_component,
        okage::RuwResource::preserve_component,okage::RuwResource::preserve_component,0});
    const auto seed=random.state;
    okage::FieldMessageWindows windows(ui,data,ruw,std::make_shared<int>(0),
        okage::FieldMessageWindows::Construction::ScriptPass,99099,globals);
    require(random.state==seed,"construction draws no RNG");
    require(windows.decoration_slot()==0&&windows.body_slot()==1,"rows construct in registry order");
    require(ui.registry[0]&&ui.registry[1]&&!ui.registry[2],"two registry windows");
    require(windows.decoration().word(0x3c00)==16&&windows.body().word(0x3c00)==12,"row capacities");
    require(globals.decoration_stream->entries.size()==2&&globals.body_stream->entries.size()==2,"constructor streams");
    require(ui.atlas_texture==99099,"11f570 *0x24f734 = id");

    okage::UiPacketBuildContext packets{fonts,0,0};
    const okage::FieldMessageWindows::Pass p{ui.resource.layout_shared(),fonts,sine,
        [&]{return random.next();},ui.resource_accumulators,packets};
    okage::DialogueOpacity decoration;decoration.set(0,.2f); //0x1a51c8 default step

    //Construction traversal: both windows run their [14,6] constructor streams.
    unsigned bootstraps=0;
    decoration.tick();sites.clear();
    windows.resource_pass(p,decoration,[&](const okage::TextPlacementHooks* hooks){require(!hooks,"bootstrap");++bootstraps;});
    require(bootstraps==1,"body bootstrap runs once");
    require(sites==join(pass(0,16),pass(0,16)),"construction: 32 + 32 constructor draws");

    //Next traversal: shared stage 0 (148958 case0/1, 148dc8 case1), then updaters.
    windows.controllers(0,-1);decoration.set(128,-128);
    const std::array<char,256> replacement{};
    okage::QuestionChoice question;
    okage::FieldMessageBodyState s;
    s.speaker="";s.replacement=&replacement;s.description="";s.player_name="Ari";
    s.status=1;s.window_type=-1;s.question=&question;s.level=10;s.money=1000;
    const okage::FieldMessageServices services{
        [&](unsigned id){return tables.system_message(id);},
        [&](std::int32_t v){return numbers.number2(v);},
        [&](std::int32_t v){return money.money_summary(v);}};
    windows.updaters(s,globals,services);
    //Original RAM (vblank 180) strings of widgets 9 and 10.
    require(text(*ruw,0,9)=="\\s[0.97,1.0]\\f[1]/\\f[1]\\c[510543555]\\c[0]10","widget9 level text: "+text(*ruw,0,9));
    require(text(*ruw,0,10)=="\\f[1]/\\c[520093567]000,00\\c[0]1,000","widget10 money text: "+text(*ruw,0,10));
    require(text(*ruw,0,15)==" "&&(ruw->widget(0,15).words[4]&1)==0,"speaker change hides widget15");
    require(windows.decoration().word(0x3bec)==2,"decoration bound in mode2");

    if(std::getenv("FMW_DUMP"))for(std::size_t i=0;i<ruw->widgets()[0].size();++i){const auto& w=ruw->widgets()[0][i];
        std::cerr<<"id "<<w.words[15]<<" type "<<w.words[5]<<" flags "<<std::hex<<w.words[4]<<std::dec;
        if(w.runtime)std::cerr<<" rgba "<<std::hex<<w.runtime->words[4]<<std::dec<<" rt0 "<<std::bit_cast<std::int32_t>(w.runtime->words[0]);
        if(w.runtime&&w.runtime->text)std::cerr<<" text "<<w.runtime->text->data();std::cerr<<"\n";}
    decoration.tick();sites.clear();
    windows.resource_pass(p,decoration,[&](const okage::TextPlacementHooks* hooks){
        require(hooks,"mode1 body");hooks->begin();hooks->tail();});
    require(sites==join(pass(15,7),pass(0,16)),"pass2: decoration 15 glyphs + 7 sentinels, body 16 sentinels; got "+runs(sites));
    require(windows.last_pass().a_draws==29&&windows.last_pass().b_draws==32,"pass accounting");

    //Third traversal: the -128 tween clamps to 0/0; the decoration is gated.
    windows.controllers(2,-1);
    windows.updaters(s,globals,services);
    decoration.tick();sites.clear();
    windows.resource_pass(p,decoration,[&](const okage::TextPlacementHooks* hooks){hooks->begin();hooks->tail();});
    require(sites==pass(0,16)&&!windows.last_pass().a_laid_out,"pass3: gated decoration, body sentinels");

    //Body glyph placement: one draw per glyph, cursor wrap into the next group.
    decoration.tick();sites.clear();
    windows.resource_pass(p,decoration,[&](const okage::TextPlacementHooks* hooks){
        hooks->begin();for(unsigned i=0;i<17;++i)hooks->glyph('a',int(i)*10,0,9,12);hooks->tail();});
    require(sites==join(pass(17,0),pass(0,16)),"17 body glyphs then 16 sentinels");
    require(windows.body().word(0x3e38)==1&&windows.body().word(0xcc0)==2,"slot/group cursor after a 16-glyph wrap");

    //A modeling disagreement with the dialogue opacity model is explicit.
    okage::DialogueOpacity wrong;wrong.set(64,0);
    rejects([&]{windows.resource_pass(p,wrong,[](const okage::TextPlacementHooks*){});},"opacity mismatch accepted");
}
}
int main(int argc,char** argv) {
    try {
        if(argc<2)throw std::runtime_error("usage: field-message-windows-tests <game-data-stage>");
        const fs::path stage=argv[1];
        if(!fs::exists(stage/"NATIVE_UI/message-windows.bin"))
            throw std::runtime_error("owned stage lacks NATIVE_UI/message-windows.bin");
        owned(stage);
        std::cout<<"field-message-windows-tests: owned construction, HUD text and pass RNG checks passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
