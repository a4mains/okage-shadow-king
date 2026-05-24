#include "okage/field_rear_banner.hpp"
#include "okage/battle_actor_math.hpp"
#include "okage/event.hpp"
#include "okage/game_state.hpp"
#include "okage/pose.hpp"
#include "okage/rng_site.hpp"
#include "okage/scene_resource.hpp"
#include "okage/ui_layout_geometry.hpp"
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fs=std::filesystem;
namespace {
void require(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
template<class F> void rejects(F f,const char* why) {
    try{f();}catch(const std::runtime_error&){return;}
    throw std::runtime_error(why);
}
std::vector<std::uint8_t> read(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("missing owned input "+path.string());
    return {std::istreambuf_iterator<char>(in),{}};
}
constexpr std::int32_t keep=okage::RuwResource::preserve_component;
std::uint32_t alpha(const okage::RuwResource& r,std::size_t group,std::int32_t id) {
    return r.widget(group,id).runtime->words[4]>>24;
}
// Pinned ELF row 0x2226f0 (tools/import_rear_banner.py), pointer words mapped
// to logical ID 1. Capacities 8/2, type 13, initial layout flag 1.
constexpr std::array<std::uint32_t,28> banner_row{
    1,25,40,1,0xc39f8000,0xc29e0000,0x43828cd9,0x3f800000,
    0,0xbfc00000,0,0,0,0x43100000,0,0,0x53f0167d,0x53f0167d,
    0x53780348,0x53780348,1,1,1,1,13,8,2,0};
std::vector<std::uint8_t> banner_bytes(const std::array<std::uint32_t,28>& row=banner_row) {
    std::vector<std::uint8_t> b;
    const auto put=[&](std::uint32_t v){for(unsigned i=0;i<4;++i)b.push_back(std::uint8_t(v>>(8*i)));};
    const auto hex=[&](std::string_view h){for(std::size_t i=0;i<h.size();i+=2)
        b.push_back(std::uint8_t(std::stoul(std::string(h.substr(i,2)),nullptr,16)));};
    for(char c:std::string_view("OKRBN1\0\0",8))b.push_back(std::uint8_t(c));
    put(1);put(28);put(1);put(16);
    hex("2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5");
    hex("9e42014fbf83588ce4728b58be4e08aaf42c81011ae7fd97536d5aeb47df71f9");
    for(auto v:row)put(v);
    for(unsigned i=0;i<4;++i)put(0);
    for(char c:std::string_view("surprise attack\0",16))b.push_back(std::uint8_t(c));
    return b;
}
std::vector<std::uint8_t> synthetic_ruw(unsigned groups) {
    std::vector<std::uint8_t> b;
    const auto put=[&](std::uint32_t v){for(unsigned i=0;i<4;++i)b.push_back(std::uint8_t(v>>(8*i)));};
    put(0);put(0);put(0);put(1);put(groups);put(0);
    for(unsigned g=0;g<groups;++g)put(2);
    for(unsigned g=0;g<groups;++g)for(unsigned id=0;id<2;++id) {
        std::array<std::uint32_t,17> w{0x3f800000,0x3f800000,id?0x80ffffffu:0x5a553228u,0x80ffffff,id?0x805fu:0x807fu,id?1u:3u,
            0xffffffff,0,0xffffffff,0,0,id?0xc2u:0x95u,id?0x1au:0xdu,0,0,id,0};
        for(auto v:w)put(v);
    }
    return b;
}
// Session-shaped owners: shared registry/coordinator, 0x1f86c0 stream,
// shared RNG with an observation-only site log.
struct Owners {
    okage::BattleUiShared ui;
    std::shared_ptr<okage::UiWindowStream> stream=std::make_shared<okage::UiWindowStream>(okage::UiWindowStream{1,{}});
    std::shared_ptr<const void> texture=std::make_shared<int>(0);
    okage::GameRandom random{0x2468aceu};
    std::vector<okage::RngSite> sites;
    okage::SineTable sine=okage::make_sine_table();
    std::shared_ptr<const std::vector<okage::Font>> fonts;
    Owners(std::shared_ptr<const std::vector<okage::Font>> f):fonts(std::move(f)) {
        okage::initialize_scene_ui(ui);
        random.observe=[](void* c,std::uint32_t){static_cast<Owners*>(c)->sites.push_back(okage::current_rng_site);};
        random.observe_context=this;
    }
    // Field resource 0x100f90 UI part at its traversal position.
    std::vector<okage::RngSite> resource(const okage::FieldRearBanner* banner) {
        sites.clear();
        std::vector<okage::UiResourceSlotBinding> bindings;
        if(banner)bindings.push_back({banner->window_slot(),0});
        (void)ui.resource.advance_resource(ui.registry,bindings,ui.resource_accumulators,fonts,sine,random,
                                           okage::initial_ui_projection());
        return sites;
    }
};
std::vector<okage::RngSite> expected_pass(unsigned glyphs,unsigned trailing) {
    std::vector<okage::RngSite> out(glyphs,okage::RngSite::UiGlyph);
    for(unsigned i=0;i<trailing;++i){out.push_back(okage::RngSite::UiIcon);out.push_back(okage::RngSite::UiSelect);}
    return out;
}
void synthetic() {
    okage::FieldRearBannerData data(banner_bytes());
    require(data.row()==banner_row&&data.atlas().size()==1,"OKRBN1 row and atlas terminator");
    auto bad=banner_bytes();bad[0xd8]='S';
    rejects([&]{okage::FieldRearBannerData d(bad);},"argument literal");
    auto other=banner_row;other[25]=9;
    rejects([&]{okage::FieldRearBannerData d(banner_bytes(other));},"capacities");
    const auto full=banner_bytes();
    rejects([&]{okage::FieldRearBannerData d(okage::Bytes(full).first(200));},"truncated");

    auto fonts=std::make_shared<const std::vector<okage::Font>>(3);
    Owners o(fonts);
    auto r=std::make_shared<okage::RuwResource>(synthetic_ruw(17));
    std::uint32_t flag=7;
    okage::FieldRearBanner banner(o.ui,o.stream,data,r,o.texture,"<%s>",flag);
    require(flag==1,"115b20 clears, then 115b50 sets 0x1f8608");
    require(o.ui.atlas_texture==-1&&o.ui.current_stream==1&&o.ui.stream_cursor==4,"11f5f8/115ad8 globals");
    require(o.stream->entries.size()==2&&o.ui.registry[banner.window_slot()],"115ad8 stream and registry window");
    require(banner.window().word(0x3c00)==8&&banner.window().word(0x3c04)==2&&banner.layout_enabled(),"11f7a0 row");
    require(o.random.state==0x2468aceu,"construction draws no RNG");
    const auto& text=*r->widget(16,1).runtime;
    require(std::string(text.text->data())=="<surprise attack>","sysmes0x72 %s format");
    require(std::bit_cast<std::int32_t>(text.words[6])==245&&text.words[7]==0x1a,"x=0x140-5*15, y kept");
    require(banner.controller_state()==0&&!banner.bound()&&banner.traversals()==0,"no callback before traversal");
    // Group1 runtime changed after load is reset by 1ab8b0(0,1,-1).
    r->set_position(1,1,9,9);
    okage::UiPartySelection party;std::array<std::uint8_t,0x1000> vars{};
    okage::set_event_variable(vars,24,4,3);
    o.ui.resource_accumulators[0]=0x1234;
    banner.traverse(party,vars,2,flag);
    require(banner.controller_state()==1&&banner.layout_enabled()&&banner.traversals()==1,"state0 then 115bd8");
    require(o.ui.resource_accumulators[0]==0,"1a9a48 resets resource-slot0 accumulator");
    require(banner.window().word(0x3bec)==2&&banner.window().opacity()==1.f,"11faa8(8.0) open and mode2 bind");
    require(r->widget(1,1).runtime->words[6]==0xc2,"1ab8b0 reset group1 class-1");
    // 1ab410 keeps the box cap in its alpha register for the later text widget.
    require(alpha(*r,16,0)==0x5a&&alpha(*r,16,1)==0x5a,"alpha128 after alpha0, capped by the box 0x5a");
    require(party.counts()[1]==1&&party.at(1,0)==0,"121688 party table refresh");
    require(o.random.state==0x2468aceu,"callbacks draw no RNG");
    // State1: no second binding/reset; 115bd8 with the flag cleared hides nothing.
    r->set_position(1,1,9,9);
    banner.traverse(party,vars,2,0);
    require(banner.controller_state()==1&&!banner.layout_enabled()&&r->widget(1,1).runtime->words[6]==9,"state1 idle");
    banner.traverse(party,vars,2,1);require(banner.layout_enabled(),"flag restores 1a5488(win,1)");
    banner.mark();require(banner.marked(),"phase10 mark");
    rejects([&]{banner.mark();},"double mark rejected");
    // A second child while the first window lives takes the next free slot;
    // 0x115b20 resets the global before 115b50 sets it.
    {
        auto fresh=std::make_shared<okage::RuwResource>(synthetic_ruw(17));
        okage::FieldRearBanner second(o.ui,o.stream,data,fresh,o.texture,"%s",flag);
        require(flag==1&&second.window_slot()!=banner.window_slot(),"115b20/115b50 order, own window");
    }
    require(o.ui.registry[banner.window_slot()]&&std::count(o.ui.registry.begin(),o.ui.registry.end(),nullptr)==7,
            "0x199458 removes only the destroyed child's window");
    const auto slot=banner.window_slot();
    rejects([&]{auto small=std::make_shared<okage::RuwResource>(synthetic_ruw(16));
        okage::FieldRearBanner b(o.ui,o.stream,data,small,o.texture,"%s",flag);},"missing group16");
    auto fresh=std::make_shared<okage::RuwResource>(synthetic_ruw(17));
    rejects([&]{okage::FieldRearBanner b(o.ui,o.stream,data,fresh,o.texture,"%d",flag);},"unreached conversion");
    rejects([&]{okage::FieldRearBanner b(o.ui,o.stream,data,fresh,o.texture,"%s%s",flag);},"second conversion");
    rejects([&]{okage::FieldRearBanner b(o.ui,o.stream,data,fresh,o.texture,std::string(113,'x')+"%s",flag);},"128-byte stack buffer");
    rejects([&]{okage::FieldRearBanner b(o.ui,o.stream,data,fresh,nullptr,"%s",flag);},"bm_if owner");
    require(std::count(o.ui.registry.begin(),o.ui.registry.end(),nullptr)==7,"rejected children leave no window");
    o.ui.registry[slot].reset();
    rejects([&]{banner.traverse(party,vars,2,1);},"traversal after the window died");
}
void owned(const fs::path& stage) {
    auto r=std::make_shared<okage::RuwResource>(read(stage/"NATIVE_UI/FLD.RUW"));
    const okage::GameDataTables tables(read(stage/"CMNDATA/XDB/BKDATA.XDB"),read(stage/"CMNDATA/XDB/ITEM.XDB"));
    require(tables.system_message(0x72)=="\\f[2]\\s[0.95,1.0]%s","owned USA sysmes 0x72");
    require(r->widgets().size()>16&&r->widgets()[16].size()==2,"owned FLD.RUW group16 box+text");
    require(alpha(*r,16,0)==0x5a&&alpha(*r,16,1)==0x80,"authored group16 alpha");
    const auto staged=stage/"NATIVE_UI/rear-banner.bin";
    const okage::FieldRearBannerData data(fs::exists(staged)?read(staged):banner_bytes());
    require(data.row()==banner_row,"staged rear banner row equals the pinned ELF row");
    auto fonts=std::make_shared<const std::vector<okage::Font>>(std::vector<okage::Font>{
        okage::read_fnt(read(stage/"CMNDATA/FONT/IQ24.FNT")),okage::read_fnt(read(stage/"CMNDATA/FONT/BMUI.FNT")),
        okage::read_fnt(read(stage/"CMNDATA/FONT/BM.FNT"))});
    Owners o(fonts);
    const auto seed=o.random.state;
    std::uint32_t flag=0;
    std::optional<okage::FieldRearBanner> banner;
    banner.emplace(o.ui,o.stream,data,r,o.texture,tables.system_message(0x72),flag);
    const auto& text=*r->widget(16,1).runtime;
    require(std::string(text.text->data())=="\\f[2]\\s[0.95,1.0]surprise attack","owned banner text");
    require(text.words[6]==245&&text.words[7]==26,"owned banner position");
    require(o.random.state==seed,"construction draws no RNG");
    okage::UiPartySelection party;std::array<std::uint8_t,0x1000> vars{};
    const auto box=okage::layout_ruw_sprites(*r,r->widget(16,0),0);
    require(box.sprites.size()==7,"owned group16 box sprite count");
    const auto pass=expected_pass(15,9);
    require(pass.size()==33,"R = 15 + 2*(16-7)");
    unsigned layouts=0;
    for(unsigned frame=1;frame<=92;++frame) {
        if(frame==92)banner->mark();          // phase10 enters with +108==0
        banner->traverse(party,vars,2,flag);  // manager+50 descendant
        const auto drawn=o.resource(&*banner);
        if(drawn!=pass)throw std::runtime_error("rear banner layout RNG pass differs at frame "+std::to_string(frame)+
            " (draws="+std::to_string(drawn.size())+")");
        ++layouts;
        if(frame==1)require(alpha(*r,16,1)==0x5a&&banner->window().opacity()==9.f,"N+1: fade-in 1+8, laid out");
    }
    require(banner->window().opacity()==128.f,"fade-in completes at 128 and keeps laying out");
    const auto glyphs=banner->glyphs((*fonts)[2]);
    require(glyphs.size()==14&&glyphs.front().code=='s'&&glyphs.back().code=='k',"diagnostic banner glyphs");
    banner.reset();                            // N+93: 0x120b48 -> 0x199458
    require(o.resource(nullptr).empty()&&std::count(o.ui.registry.begin(),o.ui.registry.end(),nullptr)==8,
            "no window, no 198390 draw after destruction");
    require(layouts==92,"92 laid-out traversals");
    okage::GameRandom reference{seed};
    for(unsigned i=0;i<92*33;++i)(void)reference.next();
    require(o.random.state==reference.state,"banner consumed exactly 92*33 draws");
    std::array<std::uint8_t,0x14> party_timer{},enemy_timer{},expected_timer{},unshifted_timer{};
    o.sites.clear();
    okage::initialize_battle_actor_timer(party_timer,10,0,1,[&]{return o.random.next();});
    require(o.sites.empty(),"rear party timer draws nothing");
    okage::initialize_battle_actor_timer(enemy_timer,10,1,1,[&]{return o.random.next();});
    okage::initialize_battle_actor_timer(expected_timer,10,1,1,[&]{return reference.next();});
    okage::GameRandom no_banner{seed};
    okage::initialize_battle_actor_timer(unshifted_timer,10,1,1,[&]{return no_banner.next();});
    require(enemy_timer==expected_timer&&o.sites.size()==1,"first battle draw follows the banner draws");
    require(enemy_timer!=unshifted_timer,"the banner draws change the first enemy timer");
    // Without a mode2 resource binding the active window is rejected, never skipped.
    std::uint32_t again=0;
    okage::FieldRearBanner unbound(o.ui,o.stream,data,r,o.texture,tables.system_message(0x72),again);
    unbound.traverse(party,vars,2,again);
    rejects([&]{(void)o.ui.resource.advance_resource(o.ui.registry,{},o.ui.resource_accumulators,fonts,o.sine,
        o.random,okage::initial_ui_projection());},"mode2 banner window needs its resource-slot binding");
}
}
int main(int argc,char** argv) {
    try {
        if(argc<2)throw std::runtime_error("usage: rear-encounter-tests <game-data-stage>");
        synthetic();
        const fs::path stage=argv[1];
        if(!fs::exists(stage/"NATIVE_UI/FLD.RUW")) {
            std::cout<<"rear-encounter-tests: synthetic tests passed; owned stage absent, skipped\n";return 0;
        }
        owned(stage);
        std::cout<<"rear-encounter-tests: synthetic and owned tests passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
